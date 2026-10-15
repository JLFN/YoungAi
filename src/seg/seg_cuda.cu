/* seg_cuda.cu — seg_ops.h 的 CUDA 实现(spark/GB10 与任何 CUDA 卡): GEMM 走 cuBLAS(f32), 其余核与 metal/seg.metal 一一对应、同一公式。
 * 行主序 C = A·Bᵀ 在 cuBLAS(列主序)里写成 Cᵀ = B·Aᵀ: cublasSgemm(T, N, N, M, K, B, K, A, lda, C, ldc)。
 * 全部用默认流(编译旗 -default-stream per-thread), seg_sync = cudaDeviceSynchronize; 进程退出即释放一切。 */
#include <cublas_v2.h>
#include <cuda_runtime.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern "C" {
#include "seg_ops.h"
}

struct seg_buf { float *p; size_t n; };
static cublasHandle_t hb;
static char g_devname[320];
static seg_buf *zeros;

#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { fprintf(stderr, "seg cuda: %s at %s:%d\n", cudaGetErrorString(e_), __FILE__, __LINE__); abort(); } } while (0)
#define THREADS 256
#define GRID(n) ((unsigned)(((n) + THREADS - 1) / THREADS))

__device__ __forceinline__ float act_f(float x, int act) {
    if (act == 1) return 0.5f * x * (1.0f + erff(x * 0.70710678118654752f));
    if (act == 2) return fmaxf(x, 0.0f);
    return x;
}

extern "C" bool seg_dev_init(char *err, size_t errn) {
    int n = 0; if (cudaGetDeviceCount(&n) != cudaSuccess || n < 1) { snprintf(err, errn, "no CUDA device"); return false; }
    cudaDeviceProp p; CK(cudaGetDeviceProperties(&p, 0)); snprintf(g_devname, sizeof g_devname, "CUDA %s", p.name);
    if (cublasCreate(&hb) != CUBLAS_STATUS_SUCCESS) { snprintf(err, errn, "cublasCreate failed"); return false; }
    float z[8192]; memset(z, 0, sizeof z); zeros = seg_buf_from(z, 8192);
    return true;
}
extern "C" void seg_dev_free(void) { seg_sync(); seg_buf_free(zeros); zeros = NULL; if (hb) cublasDestroy(hb); hb = NULL; }
extern "C" const char *seg_dev_name(void) { return g_devname; }
extern "C" void seg_sync(void) { CK(cudaDeviceSynchronize()); }

extern "C" seg_buf *seg_buf_new(size_t n) { seg_buf *b = (seg_buf *)calloc(1, sizeof *b); b->n = n; CK(cudaMalloc(&b->p, (n ? n : 1) * 4)); return b; }
extern "C" seg_buf *seg_buf_from(const float *host, size_t n) { seg_buf *b = seg_buf_new(n); CK(cudaMemcpy(b->p, host, n * 4, cudaMemcpyHostToDevice)); return b; }
extern "C" void seg_buf_read(const seg_buf *b, size_t off, float *host, size_t n) { CK(cudaMemcpy(host, b->p + off, n * 4, cudaMemcpyDeviceToHost)); }
extern "C" void seg_buf_zero(seg_buf *b, size_t off, size_t n) { CK(cudaMemsetAsync(b->p + off, 0, n * 4)); }
extern "C" void seg_buf_copy(seg_buf *dst, size_t doff, const seg_buf *src, size_t soff, size_t n) { CK(cudaMemcpyAsync(dst->p + doff, src->p + soff, n * 4, cudaMemcpyDeviceToDevice)); }
extern "C" void seg_buf_free(seg_buf *b) { if (b) { cudaFree(b->p); free(b); } }

extern "C" void seg_gemm(const seg_buf *A, size_t offA, int lda, const seg_buf *B, size_t offB, const seg_buf *bias, size_t offBias, seg_buf *C, size_t offC, int ldc, int M, int N, int K, int act) {
    if (!lda) lda = K; if (!ldc) ldc = N;
    const float one = 1.0f, zero = 0.0f;
    cublasStatus_t s = cublasSgemm(hb, CUBLAS_OP_T, CUBLAS_OP_N, N, M, K, &one, B->p + offB, K, A->p + offA, lda, &zero, C->p + offC, ldc);
    if (s != CUBLAS_STATUS_SUCCESS) { fprintf(stderr, "seg cuda: cublasSgemm %d (M %d N %d K %d)\n", (int)s, M, N, K); abort(); }
    if (bias || act) seg_bias_act(C, offC, M, N, bias ? bias : zeros, bias ? offBias : 0, act);
}

__global__ void im2col_k(const float *in, float *out, int H, int W, int C, int kh, int kw, int stride, int pad, int Hout, int Wout) {
    const int K = kh * kw * C; const size_t total = (size_t)Hout * Wout * K, id = blockIdx.x * (size_t)blockDim.x + threadIdx.x;
    if (id >= total) return;
    const int o = (int)(id / K), kk = (int)(id % K), oy = o / Wout, ox = o % Wout, ky = kk / (kw * C), kx = (kk / C) % kw, c = kk % C;
    const int iy = oy * stride - pad + ky, ix = ox * stride - pad + kx;
    out[id] = (iy < 0 || iy >= H || ix < 0 || ix >= W) ? 0.0f : in[((size_t)iy * W + ix) * C + c];
}
extern "C" void seg_im2col(const seg_buf *in, size_t off, int H, int W, int C, int kh, int kw, int stride, int pad, seg_buf *out, size_t ooff, int *Hout, int *Wout) {
    *Hout = (H + 2 * pad - kh) / stride + 1; *Wout = (W + 2 * pad - kw) / stride + 1;
    const size_t total = (size_t)*Hout * *Wout * kh * kw * C;
    im2col_k<<<GRID(total), THREADS>>>(in->p + off, out->p + ooff, H, W, C, kh, kw, stride, pad, *Hout, *Wout);
}

__global__ void dwconv3_k(const float *in, const float *w, const float *bias, float *out, int H, int W, int C, int stride, int act, int Hout, int Wout) {
    const size_t total = (size_t)Hout * Wout * C, id = blockIdx.x * (size_t)blockDim.x + threadIdx.x;
    if (id >= total) return;
    const int c = (int)(id % C), o = (int)(id / C), oy = o / Wout, ox = o % Wout;
    float s = bias[c];
    for (int ky = 0; ky < 3; ky++) { const int iy = oy * stride - 1 + ky; if (iy < 0 || iy >= H) continue;
        for (int kx = 0; kx < 3; kx++) { const int ix = ox * stride - 1 + kx; if (ix < 0 || ix >= W) continue; s += in[((size_t)iy * W + ix) * C + c] * w[c * 9 + ky * 3 + kx]; } }
    out[id] = act_f(s, act);
}
extern "C" void seg_dwconv3(const seg_buf *in, size_t off, int H, int W, int C, const seg_buf *w, size_t woff, const seg_buf *bias, size_t boff, int stride, int act, seg_buf *out, size_t ooff) {
    const int Hout = (H + 2 - 3) / stride + 1, Wout = (W + 2 - 3) / stride + 1; const size_t total = (size_t)Hout * Wout * C;
    dwconv3_k<<<GRID(total), THREADS>>>(in->p + off, w->p + woff, bias->p + boff, out->p + ooff, H, W, C, stride, act, Hout, Wout);
}

__global__ void layernorm_k(const float *x, const float *g, const float *b, float *y, int rows, int C, float eps) {
    const size_t id = blockIdx.x * (size_t)blockDim.x + threadIdx.x; if (id >= (size_t)rows) return;
    const float *r = x + id * C; float *o = y + id * C;
    float mu = 0; for (int c = 0; c < C; c++) mu += r[c]; mu /= C;
    float var = 0; for (int c = 0; c < C; c++) { const float d = r[c] - mu; var += d * d; } var /= C;
    const float inv = rsqrtf(var + eps);
    for (int c = 0; c < C; c++) o[c] = (r[c] - mu) * inv * g[c] + b[c];
}
extern "C" void seg_layernorm(const seg_buf *x, size_t off, int rows, int C, const seg_buf *g, size_t goff, const seg_buf *b, size_t boff, float eps, seg_buf *y, size_t yoff) {
    layernorm_k<<<GRID(rows), THREADS>>>(x->p + off, g->p + goff, b->p + boff, y->p + yoff, rows, C, eps);
}

__global__ void add_k(const float *a, const float *b, float *y, size_t n, int act) { const size_t id = blockIdx.x * (size_t)blockDim.x + threadIdx.x; if (id < n) y[id] = act_f(a[id] + b[id], act); }
__global__ void add_row_k(const float *x, const float *v, float *y, size_t n, int C, int act) { const size_t id = blockIdx.x * (size_t)blockDim.x + threadIdx.x; if (id < n) y[id] = act_f(x[id] + v[id % C], act); }
__global__ void act_k(float *x, size_t n, int act) { const size_t id = blockIdx.x * (size_t)blockDim.x + threadIdx.x; if (id < n) x[id] = act_f(x[id], act); }
extern "C" void seg_add(const seg_buf *a, size_t aoff, const seg_buf *b, size_t boff, int act, seg_buf *y, size_t yoff, size_t n) { add_k<<<GRID(n), THREADS>>>(a->p + aoff, b->p + boff, y->p + yoff, n, act); }
extern "C" void seg_add_row(const seg_buf *x, size_t off, int rows, int C, const seg_buf *v, size_t voff, int act, seg_buf *y, size_t yoff) { const size_t n = (size_t)rows * C; add_row_k<<<GRID(n), THREADS>>>(x->p + off, v->p + voff, y->p + yoff, n, C, act); }
extern "C" void seg_bias_act(seg_buf *x, size_t off, int rows, int C, const seg_buf *bias, size_t boff, int act) { const size_t n = (size_t)rows * C; add_row_k<<<GRID(n), THREADS>>>(x->p + off, bias->p + boff, x->p + off, n, C, act); }
extern "C" void seg_act(seg_buf *x, size_t off, size_t n, int act) { act_k<<<GRID(n), THREADS>>>(x->p + off, n, act); }

/* 注意力: 一个线程 = (b, h, 查询 n); 两遍在线 softmax, dk/dv ≤ 64 */
struct attn_p { int B, Nq, Nk, H, dk, dv; float scale; int has_bias; size_t qbs, qrs, qhs, kbs, krs, khs, vbs, vrs, vhs; };
__global__ void attention_k(const float *q, const float *k, const float *v, const float *bias, float *out, attn_p p) {
    const size_t total = (size_t)p.B * p.H * p.Nq, id = blockIdx.x * (size_t)blockDim.x + threadIdx.x;
    if (id >= total) return;
    const int n = (int)(id % p.Nq), h = (int)((id / p.Nq) % p.H), b = (int)(id / ((size_t)p.Nq * p.H));
    const float *qp = q + b * p.qbs + n * p.qrs + h * p.qhs, *kb = k + b * p.kbs + h * p.khs, *vb = v + b * p.vbs + h * p.vhs;
    const float *bi = p.has_bias ? bias + ((size_t)h * p.Nq + n) * p.Nk : bias;
    float qv[64]; for (int d = 0; d < p.dk; d++) qv[d] = qp[d] * p.scale;
    float mx = -1e30f;
    for (int j = 0; j < p.Nk; j++) { const float *kp = kb + (size_t)j * p.krs; float s = 0; for (int d = 0; d < p.dk; d++) s += qv[d] * kp[d]; if (p.has_bias) s += bi[j]; mx = fmaxf(mx, s); }
    float acc[64]; for (int d = 0; d < p.dv; d++) acc[d] = 0; float sum = 0;
    for (int j = 0; j < p.Nk; j++) { const float *kp = kb + (size_t)j * p.krs; float s = 0; for (int d = 0; d < p.dk; d++) s += qv[d] * kp[d]; if (p.has_bias) s += bi[j];
        const float e = expf(s - mx); sum += e; const float *vp = vb + (size_t)j * p.vrs; for (int d = 0; d < p.dv; d++) acc[d] += e * vp[d]; }
    float *o = out + ((size_t)(b * p.Nq + n) * p.H + h) * p.dv;
    for (int d = 0; d < p.dv; d++) o[d] = acc[d] / sum;
}
extern "C" void seg_attention(seg_view q, seg_view k, seg_view v, int B, int Nq, int Nk, int H, int dk, int dv, float scale, const seg_buf *bias, size_t bioff, seg_buf *out, size_t ooff) {
    attn_p p = { B, Nq, Nk, H, dk, dv, scale, bias != NULL, q.bs, q.rs, q.hs, k.bs, k.rs, k.hs, v.bs, v.rs, v.hs };
    const size_t total = (size_t)B * H * Nq;
    attention_k<<<GRID(total), THREADS>>>(q.buf->p + q.off, k.buf->p + k.off, v.buf->p + v.off, bias ? bias->p + bioff : zeros->p, out->p + ooff, p);
}

__global__ void window_part_k(const float *in, float *out, int H, int W, int C, int win, int nWw, int Hp, int Wp) {
    const size_t total = (size_t)Hp * Wp * C, id = blockIdx.x * (size_t)blockDim.x + threadIdx.x; if (id >= total) return;
    const int c = (int)(id % C), t = (int)(id / C), wi = t / (win * win), r = t % (win * win);
    const int y = (wi / nWw) * win + r / win, x = (wi % nWw) * win + r % win;
    out[id] = (y < H && x < W) ? in[((size_t)y * W + x) * C + c] : 0.0f;
}
__global__ void window_rev_k(const float *in, float *out, int H, int W, int C, int win, int nWw) {
    const size_t total = (size_t)H * W * C, id = blockIdx.x * (size_t)blockDim.x + threadIdx.x; if (id >= total) return;
    const int c = (int)(id % C), t = (int)(id / C), y = t / W, x = t % W;
    const int wi = (y / win) * nWw + x / win, r = (y % win) * win + x % win;
    out[id] = in[((size_t)wi * (win * win) + r) * C + c];
}
extern "C" void seg_window_part(const seg_buf *in, size_t off, int H, int W, int C, int win, seg_buf *out, size_t ooff) {
    const int Hp = (H + win - 1) / win * win, Wp = (W + win - 1) / win * win; const size_t total = (size_t)Hp * Wp * C;
    window_part_k<<<GRID(total), THREADS>>>(in->p + off, out->p + ooff, H, W, C, win, Wp / win, Hp, Wp);
}
extern "C" void seg_window_rev(const seg_buf *in, size_t off, int H, int W, int C, int win, seg_buf *out, size_t ooff) {
    const int Wp = (W + win - 1) / win * win; const size_t total = (size_t)H * W * C;
    window_rev_k<<<GRID(total), THREADS>>>(in->p + off, out->p + ooff, H, W, C, win, Wp / win);
}

__global__ void pixel_shuffle2_k(const float *in, const float *bias, float *out, int H, int W, int Cout, int act) {
    const size_t total = (size_t)4 * H * W * Cout, id = blockIdx.x * (size_t)blockDim.x + threadIdx.x; if (id >= total) return;
    const int c = (int)(id % Cout), t = (int)(id / Cout), Y = t / (2 * W), X = t % (2 * W), y = Y / 2, ky = Y % 2, x = X / 2, kx = X % 2;
    out[id] = act_f(in[((size_t)y * W + x) * (Cout * 4) + c * 4 + ky * 2 + kx] + bias[c], act);
}
extern "C" void seg_pixel_shuffle2(const seg_buf *in, size_t off, int H, int W, int Cout, const seg_buf *bias, size_t boff, int act, seg_buf *out, size_t ooff) {
    const size_t total = (size_t)4 * H * W * Cout;
    pixel_shuffle2_k<<<GRID(total), THREADS>>>(in->p + off, bias->p + boff, out->p + ooff, H, W, Cout, act);
}
