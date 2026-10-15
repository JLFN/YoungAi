/* seg_metal.m — seg_ops.h 的 Metal 实现: GEMM 走 MPSMatrixMultiplication, 其余核在 metal/seg.metal(运行时从可执行文件旁的 metal/ 目录编译)。
 *
 * 命令缓冲按批提交: 一个 cb 里连续编码多个核, 每 SEG_BATCH 个算子或要读回/同步时提交 —— 每个算子一个 cb 的做法 500 个算子要多付几十毫秒。
 * MPS 要求矩阵在 buffer 里的偏移 16 字节对齐, 所以 seg_gemm 的三个偏移都必须是 4 个 float 的倍数(调用方按 16 float 对齐分配, 见 seg_model.c)。
 * 所有 buffer 用 shared 存储(Apple Silicon 统一内存): 上传/下载就是 memcpy, 读回前等 GPU 做完。 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>
#include <mach-o/dyld.h>
#include <libgen.h>
#include <stdio.h>
#include <string.h>
#include "seg_ops.h"

#define SEG_BATCH 48

struct seg_buf { id<MTLBuffer> b; size_t n; };

static id<MTLDevice> dev;
static id<MTLCommandQueue> queue;
static id<MTLCommandBuffer> cb;
static id<MTLComputeCommandEncoder> enc;
static int pending;
static NSMutableDictionary<NSString *, id<MTLComputePipelineState>> *pipes;
static NSMutableDictionary<NSString *, MPSMatrixMultiplication *> *mms;
static seg_buf *zeros;   /* 8192 个 0: 只要激活不要偏置时当偏置用 */
static char g_devname[128];

static id<MTLComputeCommandEncoder> E(void) {
    if (!cb) cb = [queue commandBuffer];
    if (!enc) enc = [cb computeCommandEncoder];
    return enc;
}
static void end_enc(void) { if (enc) { [enc endEncoding]; enc = nil; } }
static void flush(void) { end_enc(); if (cb) { [cb commit]; cb = nil; } pending = 0; }
static void tick(void) { if (++pending >= SEG_BATCH) flush(); }
void seg_sync(void) { end_enc(); if (cb) { [cb commit]; [cb waitUntilCompleted]; cb = nil; } pending = 0; }

static void run(const char *name, const void *params, size_t plen, size_t total, __strong id<MTLBuffer> *bufs, const size_t *offs, int nb) {
    id<MTLComputePipelineState> ps = pipes[@(name)];
    id<MTLComputeCommandEncoder> e = E();
    [e setComputePipelineState:ps];
    for (int i = 0; i < nb; i++) [e setBuffer:bufs[i] offset:offs[i] * 4 atIndex:(NSUInteger)i];
    [e setBytes:params length:plen atIndex:(NSUInteger)nb];
    NSUInteger tg = ps.maxTotalThreadsPerThreadgroup; if (tg > 256) tg = 256;
    [e dispatchThreads:MTLSizeMake(total, 1, 1) threadsPerThreadgroup:MTLSizeMake(tg, 1, 1)];
    tick();
}

bool seg_dev_init(char *err, size_t errn) {
    @autoreleasepool {
        dev = MTLCreateSystemDefaultDevice();
        if (!dev) { snprintf(err, errn, "no Metal device"); return false; }
        queue = [dev newCommandQueue];
        char exe[4096]; uint32_t n = sizeof exe; NSString *src = nil;
        if (_NSGetExecutablePath(exe, &n) == 0) {
            char *dir = dirname(exe); NSString *p = [NSString stringWithFormat:@"%s/metal/seg.metal", dir];
            src = [NSString stringWithContentsOfFile:p encoding:NSUTF8StringEncoding error:nil];
        }
        if (!src) src = [NSString stringWithContentsOfFile:@"metal/seg.metal" encoding:NSUTF8StringEncoding error:nil];
        if (!src) { snprintf(err, errn, "cannot read metal/seg.metal (next to the executable or in the working directory)"); return false; }
        NSError *e = nil; MTLCompileOptions *opt = [MTLCompileOptions new];
        /* erf/exp 要全精度: 掩膜 logit 的符号就是边界 */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        if (@available(macOS 15.0, *)) opt.mathMode = MTLMathModeSafe; else opt.fastMathEnabled = NO;
#pragma clang diagnostic pop
        id<MTLLibrary> lib = [dev newLibraryWithSource:src options:opt error:&e];
        if (!lib) { snprintf(err, errn, "seg.metal compile: %s", e.localizedDescription.UTF8String); return false; }
        pipes = [NSMutableDictionary new]; mms = [NSMutableDictionary new];
        for (NSString *fn in lib.functionNames) {
            id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:fn] error:&e];
            if (!ps) { snprintf(err, errn, "pipeline %s: %s", fn.UTF8String, e.localizedDescription.UTF8String); return false; }
            pipes[fn] = ps;
        }
        snprintf(g_devname, sizeof g_devname, "Metal %s", dev.name.UTF8String);
        float z[8192] = {0}; zeros = seg_buf_from(z, 8192);
        return true;
    }
}
void seg_dev_free(void) { seg_sync(); seg_buf_free(zeros); zeros = NULL; pipes = nil; mms = nil; queue = nil; dev = nil; }
const char *seg_dev_name(void) { return g_devname; }

seg_buf *seg_buf_new(size_t n) {
    seg_buf *b = calloc(1, sizeof *b); b->n = n;
    b->b = [dev newBufferWithLength:(n ? n : 1) * 4 options:MTLResourceStorageModeShared];
    return b;
}
seg_buf *seg_buf_from(const float *host, size_t n) { seg_buf *b = seg_buf_new(n); memcpy(b->b.contents, host, n * 4); return b; }
void seg_buf_read(const seg_buf *b, size_t off, float *host, size_t n) { seg_sync(); memcpy(host, (const float *)b->b.contents + off, n * 4); }
void seg_buf_zero(seg_buf *b, size_t off, size_t n) { end_enc(); if (!cb) cb = [queue commandBuffer]; id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder]; [bl fillBuffer:b->b range:NSMakeRange(off * 4, n * 4) value:0]; [bl endEncoding]; tick(); }
void seg_buf_copy(seg_buf *dst, size_t doff, const seg_buf *src, size_t soff, size_t n) { end_enc(); if (!cb) cb = [queue commandBuffer]; id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder]; [bl copyFromBuffer:src->b sourceOffset:soff * 4 toBuffer:dst->b destinationOffset:doff * 4 size:n * 4]; [bl endEncoding]; tick(); }
void seg_buf_free(seg_buf *b) { if (b) { b->b = nil; free(b); } }

void seg_gemm(const seg_buf *A, size_t offA, int lda, const seg_buf *B, size_t offB, const seg_buf *bias, size_t offBias, seg_buf *C, size_t offC, int ldc, int M, int N, int K, int act) {
    @autoreleasepool {
        if (!lda) lda = K; if (!ldc) ldc = N;
        if ((offA | offB | offC) & 3) { fprintf(stderr, "seg_gemm: offsets must be multiples of 4 floats (MPS alignment)\n"); abort(); }
        end_enc(); if (!cb) cb = [queue commandBuffer];
        NSString *key = [NSString stringWithFormat:@"%d/%d/%d", M, N, K];
        MPSMatrixMultiplication *mm = mms[key];
        if (!mm) { mm = [[MPSMatrixMultiplication alloc] initWithDevice:dev transposeLeft:NO transposeRight:YES resultRows:(NSUInteger)M resultColumns:(NSUInteger)N interiorColumns:(NSUInteger)K alpha:1.0 beta:0.0]; mms[key] = mm; }
        MPSMatrix *ma = [[MPSMatrix alloc] initWithBuffer:A->b offset:offA * 4 descriptor:[MPSMatrixDescriptor matrixDescriptorWithRows:(NSUInteger)M columns:(NSUInteger)K rowBytes:(NSUInteger)lda * 4 dataType:MPSDataTypeFloat32]];
        MPSMatrix *mb = [[MPSMatrix alloc] initWithBuffer:B->b offset:offB * 4 descriptor:[MPSMatrixDescriptor matrixDescriptorWithRows:(NSUInteger)N columns:(NSUInteger)K rowBytes:(NSUInteger)K * 4 dataType:MPSDataTypeFloat32]];
        MPSMatrix *mc = [[MPSMatrix alloc] initWithBuffer:C->b offset:offC * 4 descriptor:[MPSMatrixDescriptor matrixDescriptorWithRows:(NSUInteger)M columns:(NSUInteger)N rowBytes:(NSUInteger)ldc * 4 dataType:MPSDataTypeFloat32]];
        [mm encodeToCommandBuffer:cb leftMatrix:ma rightMatrix:mb resultMatrix:mc];
        tick();
        if (bias || act) seg_bias_act(C, offC, M, N, bias ? bias : zeros, bias ? offBias : 0, act);   /* ldc == N 时才成立: 带行步长的 C 不配偏置(调用方保证) */
    }
}

void seg_im2col(const seg_buf *in, size_t off, int H, int W, int C, int kh, int kw, int stride, int pad, seg_buf *out, size_t ooff, int *Hout, int *Wout) {
    struct { int H, W, C, kh, kw, stride, pad, Hout, Wout; } p = { H, W, C, kh, kw, stride, pad, (H + 2 * pad - kh) / stride + 1, (W + 2 * pad - kw) / stride + 1 };
    *Hout = p.Hout; *Wout = p.Wout;
    id<MTLBuffer> bufs[] = { in->b, out->b }; const size_t offs[] = { off, ooff };
    run("seg_im2col_k", &p, sizeof p, (size_t)p.Hout * p.Wout * kh * kw * C, bufs, offs, 2);
}
void seg_dwconv3(const seg_buf *in, size_t off, int H, int W, int C, const seg_buf *w, size_t woff, const seg_buf *bias, size_t boff, int stride, int act, seg_buf *out, size_t ooff) {
    struct { int H, W, C, stride, act, Hout, Wout; } p = { H, W, C, stride, act, (H + 2 - 3) / stride + 1, (W + 2 - 3) / stride + 1 };
    id<MTLBuffer> bufs[] = { in->b, w->b, bias->b, out->b }; const size_t offs[] = { off, woff, boff, ooff };
    run("seg_dwconv3_k", &p, sizeof p, (size_t)p.Hout * p.Wout * C, bufs, offs, 4);
}
void seg_layernorm(const seg_buf *x, size_t off, int rows, int C, const seg_buf *g, size_t goff, const seg_buf *b, size_t boff, float eps, seg_buf *y, size_t yoff) {
    struct { int rows, C; float eps; } p = { rows, C, eps };
    id<MTLBuffer> bufs[] = { x->b, g->b, b->b, y->b }; const size_t offs[] = { off, goff, boff, yoff };
    run("seg_layernorm_k", &p, sizeof p, (size_t)rows, bufs, offs, 4);
}
void seg_add(const seg_buf *a, size_t aoff, const seg_buf *b, size_t boff, int act, seg_buf *y, size_t yoff, size_t n) {
    struct { unsigned n; int act, C; } p = { (unsigned)n, act, 1 };
    id<MTLBuffer> bufs[] = { a->b, b->b, y->b }; const size_t offs[] = { aoff, boff, yoff };
    run("seg_add_k", &p, sizeof p, n, bufs, offs, 3);
}
void seg_add_row(const seg_buf *x, size_t off, int rows, int C, const seg_buf *v, size_t voff, int act, seg_buf *y, size_t yoff) {
    struct { unsigned n; int act, C; } p = { (unsigned)((size_t)rows * C), act, C };
    id<MTLBuffer> bufs[] = { x->b, v->b, y->b }; const size_t offs[] = { off, voff, yoff };
    run("seg_add_row_k", &p, sizeof p, (size_t)rows * C, bufs, offs, 3);
}
void seg_bias_act(seg_buf *x, size_t off, int rows, int C, const seg_buf *bias, size_t boff, int act) {
    struct { unsigned n; int act, C; } p = { (unsigned)((size_t)rows * C), act, C };
    id<MTLBuffer> bufs[] = { x->b, bias->b }; const size_t offs[] = { off, boff };
    run("seg_bias_act_k", &p, sizeof p, (size_t)rows * C, bufs, offs, 2);
}
void seg_act(seg_buf *x, size_t off, size_t n, int act) {
    struct { unsigned n; int act, C; } p = { (unsigned)n, act, 1 };
    id<MTLBuffer> bufs[] = { x->b }; const size_t offs[] = { off };
    run("seg_act_k", &p, sizeof p, n, bufs, offs, 1);
}
void seg_attention(seg_view q, seg_view k, seg_view v, int B, int Nq, int Nk, int H, int dk, int dv, float scale, const seg_buf *bias, size_t bioff, seg_buf *out, size_t ooff) {
    struct { int B, Nq, Nk, H, dk, dv; float scale; int has_bias; unsigned qbs, qrs, qhs, kbs, krs, khs, vbs, vrs, vhs; } p =
        { B, Nq, Nk, H, dk, dv, scale, bias != NULL, (unsigned)q.bs, (unsigned)q.rs, (unsigned)q.hs, (unsigned)k.bs, (unsigned)k.rs, (unsigned)k.hs, (unsigned)v.bs, (unsigned)v.rs, (unsigned)v.hs };
    id<MTLBuffer> bufs[] = { q.buf->b, k.buf->b, v.buf->b, bias ? bias->b : zeros->b, out->b }; const size_t offs[] = { q.off, k.off, v.off, bias ? bioff : 0, ooff };
    run("seg_attention_k", &p, sizeof p, (size_t)B * H * Nq, bufs, offs, 5);
}
void seg_window_part(const seg_buf *in, size_t off, int H, int W, int C, int win, seg_buf *out, size_t ooff) {
    const int Hp = (H + win - 1) / win * win, Wp = (W + win - 1) / win * win;
    struct { int H, W, C, win, nWw, Hp, Wp; } p = { H, W, C, win, Wp / win, Hp, Wp };
    id<MTLBuffer> bufs[] = { in->b, out->b }; const size_t offs[] = { off, ooff };
    run("seg_window_part_k", &p, sizeof p, (size_t)Hp * Wp * C, bufs, offs, 2);
}
void seg_window_rev(const seg_buf *in, size_t off, int H, int W, int C, int win, seg_buf *out, size_t ooff) {
    const int Hp = (H + win - 1) / win * win, Wp = (W + win - 1) / win * win;
    struct { int H, W, C, win, nWw, Hp, Wp; } p = { H, W, C, win, Wp / win, Hp, Wp };
    id<MTLBuffer> bufs[] = { in->b, out->b }; const size_t offs[] = { off, ooff };
    run("seg_window_rev_k", &p, sizeof p, (size_t)H * W * C, bufs, offs, 2);
}
void seg_pixel_shuffle2(const seg_buf *in, size_t off, int H, int W, int Cout, const seg_buf *bias, size_t boff, int act, seg_buf *out, size_t ooff) {
    struct { int H, W, Cout, act; } p = { H, W, Cout, act };
    id<MTLBuffer> bufs[] = { in->b, bias->b, out->b }; const size_t offs[] = { off, boff, ooff };
    run("seg_pixel_shuffle2_k", &p, sizeof p, (size_t)4 * H * W * Cout, bufs, offs, 3);
}
