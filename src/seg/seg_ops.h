/* seg_ops.h — 分割模型的设备算子契约: 一份接口, Metal(seg_metal.m + metal/seg.metal)与 CUDA(seg_cuda.cu)各一份实现。
 *
 * 约定: 所有张量都是 f32、行主序、"token 优先"布局 —— 图像特征存成 [H*W][C](NHWC), 和 torch Linear 的 [N][C] 一个样子, 卷积走
 * im2col + GEMM, 1×1 卷积就是 GEMM。权重按 torch Linear 布局 [out][in] 上传, GEMM 一律算 C = A·Bᵀ(+bias, 可带激活)。
 * 偏移全按 float 个数计。act: 0 无 / 1 GELU(erf 精确版, 与 torch.nn.GELU 默认同) / 2 ReLU。
 * 为什么不复用引擎的 ds4_gpu.h: 那是 LLM 的 bf16/量化核契约, 没有卷积/窗口注意力; 这个模型 42 GFLOPs 全 f32 就够, 独立一套小得多。 */
#ifndef SEG_OPS_H
#define SEG_OPS_H
#include <stdbool.h>
#include <stddef.h>

typedef struct seg_buf seg_buf;

bool seg_dev_init(char *err, size_t errn);   /* 建设备/队列/编译核; ds4-seg 进程只调一次 */
void seg_dev_free(void);
const char *seg_dev_name(void);

seg_buf *seg_buf_new(size_t n);                       /* n 个 float, 内容未定义 */
seg_buf *seg_buf_from(const float *host, size_t n);   /* 上传 */
void seg_buf_read(const seg_buf *b, size_t off, float *host, size_t n);   /* 下载(同步) */
void seg_buf_zero(seg_buf *b, size_t off, size_t n);
void seg_buf_copy(seg_buf *dst, size_t doff, const seg_buf *src, size_t soff, size_t n);
void seg_buf_free(seg_buf *b);
void seg_sync(void);

/* C[M×N] = A[M×K] · B[N×K]ᵀ (+ bias[N]) (+act); ld* = 行步长(0 = 紧凑) */
void seg_gemm(const seg_buf *A, size_t offA, int lda, const seg_buf *B, size_t offB, const seg_buf *C_bias, size_t offBias, seg_buf *C, size_t offC, int ldc, int M, int N, int K, int act);

/* im2col: in [H][W][Cin] → out [Hout*Wout][kh*kw*Cin], 核元素序 (ky, kx, cin); 补零边 pad, 步长 stride */
void seg_im2col(const seg_buf *in, size_t off, int H, int W, int C, int kh, int kw, int stride, int pad, seg_buf *out, size_t ooff, int *Hout, int *Wout);
/* 深度 3×3 卷积(NHWC, 补 1 边): w [C][9] 序 (ky,kx), bias [C]; stride 1/2; 出 [Hout*Wout][C] */
void seg_dwconv3(const seg_buf *in, size_t off, int H, int W, int C, const seg_buf *w, size_t woff, const seg_buf *bias, size_t boff, int stride, int act, seg_buf *out, size_t ooff);
/* LayerNorm 逐行(最后一维 C): y = (x−μ)/√(σ²+eps)·g + b; 也是 LayerNorm2d(NHWC 下逐 token 就是逐通道归一) */
void seg_layernorm(const seg_buf *x, size_t off, int rows, int C, const seg_buf *g, size_t goff, const seg_buf *b, size_t boff, float eps, seg_buf *y, size_t yoff);
/* y = a + b (+act); n 个元素 */
void seg_add(const seg_buf *a, size_t aoff, const seg_buf *b, size_t boff, int act, seg_buf *y, size_t yoff, size_t n);
/* y[r][c] = x[r][c] + v[c] (+act): 行广播加 */
void seg_add_row(const seg_buf *x, size_t off, int rows, int C, const seg_buf *v, size_t voff, int act, seg_buf *y, size_t yoff);
/* 就地 x = act(x + bias[c]) */
void seg_bias_act(seg_buf *x, size_t off, int rows, int C, const seg_buf *bias, size_t boff, int act);
/* 就地 x = act(x), n 个元素 */
void seg_act(seg_buf *x, size_t off, size_t n, int act);
/* 多头注意力: q/k/v 各自 [B][Nq|Nk][...]: 元素 (b, n, h, d) 在 base + b*bs + n*rs + h*hs + d; out [B][Nq][H*dv] 紧凑;
 * bias(可空): [H][Nq][Nk] 加在缩放后的分数上(TinyViT 的 attention_biases 展开表); softmax 沿 Nk */
typedef struct { const seg_buf *buf; size_t off; size_t bs, rs, hs; } seg_view;
void seg_attention(seg_view q, seg_view k, seg_view v, int B, int Nq, int Nk, int H, int dk, int dv, float scale, const seg_buf *bias, size_t bioff, seg_buf *out, size_t ooff);
/* 窗口切分/还原(TinyViT): in [H][W][C] → out [nWh*nWw*win*win][C], 越界补零; rev 反向(越界丢) */
void seg_window_part(const seg_buf *in, size_t off, int H, int W, int C, int win, seg_buf *out, size_t ooff);
void seg_window_rev(const seg_buf *in, size_t off, int H, int W, int C, int win, seg_buf *out, size_t ooff);
/* ConvTranspose2d k2 s2 的后半: GEMM 出的 [H*W][Cout*4](列序 co*4 + ky*2 + kx) → [2H*2W][Cout] (+bias) (+act) */
void seg_pixel_shuffle2(const seg_buf *in, size_t off, int H, int W, int Cout, const seg_buf *bias, size_t boff, int act, seg_buf *out, size_t ooff);

#endif
