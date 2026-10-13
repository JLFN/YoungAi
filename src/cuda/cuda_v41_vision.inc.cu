/* cuda_v41_vision.inc.cu — ds4_cuda.cu 分片: 视觉塔(DeepSeek-ViT + aligner)缺的几件原语(2026-10-13)。契约见 ds4_gpu_v41.h。
 *
 * 为什么只有这几件: ViT 的矩阵乘(patch_embed / wqkv / wo / w1 / w2 / aligner 两层)全是 BF16 原件, 走现成的
 * ds4_gpu_v41_matmul_bf16_tensor(cuBLAS); RMSNorm / 残差加 / bf16 舍入也都有。缺的是 Linear 的 bias、2D RoPE、
 * 无掩码的稠密注意力、不截断的 SwiGLU、GELU、aligner 的 3×3 像素重排, 以及把视觉行塞进词嵌入的行替换。
 * 口径: f32 算、官方 bf16 模块边界处舍 bf16(v41_bf16r), 与主干同一条纪律。
 * 出错会怎样: 这里任何一件算错都不报错 —— 模型照样生成, 只是看图答非所问。门 = tools/vision_golden.py 的金标
 * (官方 vision.py 同一张图的 aligner 输出)逐行 cos/最大绝对差。 */

static v41_scratch g_vis_qkv16, g_vis_s, g_vis_p;

__global__ static void v41_bias_add_kernel(float *x, const float *b, uint64_t n, uint32_t dim) {
    v41_pdl_wait();
    const uint64_t i = (uint64_t)blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    x[i] = v41_bf16r(x[i] + b[i % dim]);
}
int ds4_gpu_v41_bias_add_dev_tensor(ds4_gpu_tensor *x, const ds4_gpu_tensor *bias, uint32_t n_tok, uint32_t dim) {
    if (!x || !bias || !n_tok || !dim || bias->bytes < (uint64_t)dim * 4) return 0;
    const uint64_t n = (uint64_t)n_tok * dim;
    v41_bias_add_kernel<<<(unsigned)((n + 255) / 256), 256, 0, g_cur_stream>>>((float *)x->ptr, (const float *)bias->ptr, n, dim);
    return cuda_ok(cudaGetLastError(), "vision bias add");
}
/* 权重已在设备张量里(来图时从 SSD 读进来的临时副本): 直接走 bf16 GEMM 主体, 不碰模型映射 */
int ds4_gpu_v41_matmul_bf16_dev_tensor(ds4_gpu_tensor *out, const ds4_gpu_tensor *W, uint64_t in_dim, uint64_t out_dim,
                                       const ds4_gpu_tensor *x, uint32_t n_tok) {
    if (!out || !W || !x || !n_tok || W->bytes < in_dim * out_dim * 2) return 0;
    return v41_bf16_matmul_ptr((const __nv_bfloat16 *)W->ptr, in_dim, out_dim, x, out, n_tok);
}
int ds4_gpu_v41_rms_norm_dev_tensor(ds4_gpu_tensor *out, const ds4_gpu_tensor *x, const ds4_gpu_tensor *w, uint32_t dim, uint32_t n_tok, float eps) {
    if (!out || !x || !w || w->bytes < (uint64_t)dim * 4) return 0;
    v41_rms_norm_kernel<<<n_tok, 256, 0, g_cur_stream>>>((float *)out->ptr, (const float *)x->ptr, (const float *)w->ptr, dim, eps);
    return cuda_ok(cudaGetLastError(), "vision rms norm");
}

/* 一线程一 (token, 头, j<rope_dim): 同时转 q 与 k 的第 j / j+rope_dim 两维。频率表与官方 get_vision_cos_sin 同式:
 * inv_freq[i] = theta^(-2i/rope_dim), i < rope_dim/2; freqs = [行号·inv_freq | 列号·inv_freq]。 */
__global__ static void v41_vis_rope_kernel(float *qkv, uint32_t n_tok, uint32_t n_w, uint32_t n_head, uint32_t head_dim, float theta) {
    v41_pdl_wait();
    const uint32_t rd = head_dim / 2u, nf = rd / 2u;
    const uint64_t total = (uint64_t)n_tok * n_head * rd;
    const uint64_t i = (uint64_t)blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= total) return;
    const uint32_t j = (uint32_t)(i % rd), h = (uint32_t)((i / rd) % n_head), t = (uint32_t)(i / ((uint64_t)rd * n_head));
    const uint32_t fi = j < nf ? j : j - nf;
    const float pos = j < nf ? (float)(t / n_w) : (float)(t % n_w);
    const float inv = 1.0f / powf(theta, (float)(2u * fi) / (float)rd);
    const float f = pos * inv, c = cosf(f), s = sinf(f);
    const uint64_t row = (uint64_t)t * 3u * n_head * head_dim;
    #pragma unroll
    for (uint32_t part = 0; part < 2u; part++) {   /* 0 = q, 1 = k */
        float *b = qkv + row + (uint64_t)part * n_head * head_dim + (uint64_t)h * head_dim;
        const float x1 = b[j], x2 = b[j + rd];
        b[j] = v41_bf16r(x1 * c - x2 * s);
        b[j + rd] = v41_bf16r(x2 * c + x1 * s);
    }
}
int ds4_gpu_v41_vision_rope_tensor(ds4_gpu_tensor *qkv, uint32_t n_tok, uint32_t n_w, uint32_t n_head, uint32_t head_dim, float theta) {
    if (!qkv || !n_tok || !n_w || !n_head || (head_dim % 4u) != 0u) return 0;
    const uint64_t total = (uint64_t)n_tok * n_head * (head_dim / 2u);
    v41_vis_rope_kernel<<<(unsigned)((total + 255) / 256), 256, 0, g_cur_stream>>>((float *)qkv->ptr, n_tok, n_w, n_head, head_dim, theta);
    return cuda_ok(cudaGetLastError(), "vision rope");
}

/* 行 softmax: 一 block 一行(nk 个分), 先乘 scale, 减最大值, exp, 归一, 出 bf16(P 喂张量核做 P·V) */
__global__ static void v41_vis_softmax_kernel(__nv_bfloat16 *P, const float *S, uint32_t nk, float scale) {
    v41_pdl_wait();
    __shared__ float red[256];
    const float *s = S + (uint64_t)blockIdx.x * nk;
    __nv_bfloat16 *p = P + (uint64_t)blockIdx.x * nk;
    float m = -INFINITY;
    for (uint32_t k = threadIdx.x; k < nk; k += blockDim.x) m = fmaxf(m, s[k] * scale);
    red[threadIdx.x] = m; __syncthreads();
    for (uint32_t o = 128; o > 0; o >>= 1) { if (threadIdx.x < o) red[threadIdx.x] = fmaxf(red[threadIdx.x], red[threadIdx.x + o]); __syncthreads(); }
    m = red[0]; __syncthreads();
    float sum = 0.f;
    for (uint32_t k = threadIdx.x; k < nk; k += blockDim.x) sum += expf(s[k] * scale - m);
    red[threadIdx.x] = sum; __syncthreads();
    for (uint32_t o = 128; o > 0; o >>= 1) { if (threadIdx.x < o) red[threadIdx.x] += red[threadIdx.x + o]; __syncthreads(); }
    const float inv = 1.0f / red[0];
    for (uint32_t k = threadIdx.x; k < nk; k += blockDim.x) p[k] = __float2bfloat16(expf(s[k] * scale - m) * inv);
}
/* 查询分块的内存上限: S(f32) + P(bf16) 每块 H·qc·n·6 B。1024-token 的图 n ≈ 8600、H 16 ⇒ 每行 0.8 MB, 封 192 MB ⇒ qc ≈ 230, 一层 ~38 块。
 * 选 192 MB 的理由: 与主干预填 bf16 暂存同量级, 不让看图把内存峰值抬到看门狗线上。 */
#define V41_VIS_ATTN_BYTES (192ull << 20)
int ds4_gpu_v41_dense_attn_tensor(ds4_gpu_tensor *o, const ds4_gpu_tensor *qkv, uint32_t n_tok, uint32_t n_head, uint32_t head_dim) {
    if (!o || !qkv || !g_cublas_ready || !n_tok || !n_head || !head_dim) return 0;
    const uint32_t HD = n_head * head_dim, W = 3u * HD;
    const uint64_t xn = (uint64_t)n_tok * W;
    __nv_bfloat16 *q16 = (__nv_bfloat16 *)v41_grow(&g_vis_qkv16, xn * 2, "vision qkv bf16");
    if (!q16) return 0;
    v41_x_to_bf16_kernel<<<(unsigned)((xn + 255) / 256), 256, 0, g_cur_stream>>>(q16, (const float *)qkv->ptr, xn);
    if (!cuda_ok(cudaGetLastError(), "vision qkv→bf16")) return 0;
    uint64_t qc = V41_VIS_ATTN_BYTES / ((uint64_t)n_head * n_tok * 6u);
    if (qc < 1) qc = 1;
    if (qc > n_tok) qc = n_tok;
    float *S = (float *)v41_grow(&g_vis_s, (uint64_t)n_head * qc * n_tok * 4, "vision attn S");
    __nv_bfloat16 *P = (__nv_bfloat16 *)v41_grow(&g_vis_p, (uint64_t)n_head * qc * n_tok * 2, "vision attn P");
    if (!S || !P) return 0;
    const float alpha = 1.0f, beta = 0.0f, scale = 1.0f / sqrtf((float)head_dim);
    (void)cublasSetStream(g_cublas, v41_cublas_stream());
    for (uint32_t q0 = 0; q0 < n_tok; q0 += (uint32_t)qc) {
        const uint32_t nq = (uint32_t)(n_tok - q0 < qc ? n_tok - q0 : qc);
        /* S[h][q][k] = Σ_d K[k][d]·Q[q][d]: 列主序看是 C(nk × nq) = Kᵀ(nk × D) · Q(D × nq), K/Q 的列距都是整行 W */
        cublasStatus_t st = cublasGemmStridedBatchedEx(g_cublas, CUBLAS_OP_T, CUBLAS_OP_N, (int)n_tok, (int)nq, (int)head_dim, &alpha,
                                                       q16 + HD, CUDA_R_16BF, (int)W, (long long)head_dim,
                                                       q16 + (uint64_t)q0 * W, CUDA_R_16BF, (int)W, (long long)head_dim, &beta,
                                                       S, CUDA_R_32F, (int)n_tok, (long long)n_tok * nq, (int)n_head,
                                                       CUBLAS_COMPUTE_32F, CUBLAS_GEMM_DEFAULT);
        if (!cublas_ok(st, "vision attn QKᵀ")) return 0;
        v41_vis_softmax_kernel<<<(unsigned)(n_head * nq), 256, 0, g_cur_stream>>>(P, S, n_tok, scale);
        if (!cuda_ok(cudaGetLastError(), "vision attn softmax")) return 0;
        /* O[h][q][d] = Σ_k V[k][d]·P[q][k]: 列主序 C(D × nq) = V(D × nk) · P(nk × nq); O 直接落 o 的 [q0.., h·D..] 段(列距 HD) */
        st = cublasGemmStridedBatchedEx(g_cublas, CUBLAS_OP_N, CUBLAS_OP_N, (int)head_dim, (int)nq, (int)n_tok, &alpha,
                                        q16 + 2u * HD, CUDA_R_16BF, (int)W, (long long)head_dim,
                                        P, CUDA_R_16BF, (int)n_tok, (long long)n_tok * nq, &beta,
                                        (float *)o->ptr + (uint64_t)q0 * HD, CUDA_R_32F, (int)HD, (long long)head_dim, (int)n_head,
                                        CUBLAS_COMPUTE_32F, CUBLAS_GEMM_DEFAULT);
        if (!cublas_ok(st, "vision attn PV")) return 0;
    }
    return ds4_gpu_v41_round_bf16_tensor(o, (uint64_t)n_tok * HD);
}

__global__ static void v41_swiglu_pair_kernel(float *h, const float *gu, uint64_t n, uint32_t mid) {
    v41_pdl_wait();
    const uint64_t i = (uint64_t)blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    const uint64_t t = i / mid, j = i % mid;
    const float g = gu[t * 2u * mid + j], u = gu[t * 2u * mid + mid + j];
    h[i] = v41_bf16r((g / (1.0f + expf(-g))) * u);
}
int ds4_gpu_v41_swiglu_pair_tensor(ds4_gpu_tensor *h, const ds4_gpu_tensor *gu, uint32_t n_tok, uint32_t mid) {
    if (!h || !gu || !n_tok || !mid) return 0;
    const uint64_t n = (uint64_t)n_tok * mid;
    v41_swiglu_pair_kernel<<<(unsigned)((n + 255) / 256), 256, 0, g_cur_stream>>>((float *)h->ptr, (const float *)gu->ptr, n, mid);
    return cuda_ok(cudaGetLastError(), "vision swiglu");
}

__global__ static void v41_gelu_kernel(float *x, uint64_t n) {
    v41_pdl_wait();
    const uint64_t i = (uint64_t)blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    const float v = x[i];
    x[i] = v41_bf16r(0.5f * v * (1.0f + erff(v * 0.70710678118654752f)));
}
int ds4_gpu_v41_gelu_tensor(ds4_gpu_tensor *x, uint64_t n) {
    if (!x || !n) return 0;
    v41_gelu_kernel<<<(unsigned)((n + 255) / 256), 256, 0, g_cur_stream>>>((float *)x->ptr, n);
    return cuda_ok(cudaGetLastError(), "vision gelu");
}

__global__ static void v41_vis_unfold_kernel(float *out, const float *x, uint32_t n_h, uint32_t n_w, uint32_t dim, uint32_t r, uint32_t ow, uint64_t total) {
    v41_pdl_wait();
    const uint64_t i = (uint64_t)blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= total) return;
    const uint32_t cols = dim * r * r;
    const uint32_t col = (uint32_t)(i % cols), cell = (uint32_t)(i / cols);
    const uint32_t oi = cell / ow, oj = cell % ow;
    const uint32_t c = col / (r * r), rem = col % (r * r), kh = rem / r, kw = rem % r;
    const uint32_t pi = oi * r + kh, pj = oj * r + kw;
    out[i] = (pi < n_h && pj < n_w) ? x[((uint64_t)pi * n_w + pj) * dim + c] : 0.0f;
}
int ds4_gpu_v41_vision_unfold_tensor(ds4_gpu_tensor *out, const ds4_gpu_tensor *x, uint32_t n_h, uint32_t n_w, uint32_t dim, uint32_t r) {
    if (!out || !x || !n_h || !n_w || !dim || !r) return 0;
    const uint32_t oh = (n_h + r - 1u) / r, ow = (n_w + r - 1u) / r;
    const uint64_t total = (uint64_t)oh * ow * dim * r * r;
    v41_vis_unfold_kernel<<<(unsigned)((total + 255) / 256), 256, 0, g_cur_stream>>>((float *)out->ptr, (const float *)x->ptr, n_h, n_w, dim, r, ow, total);
    return cuda_ok(cudaGetLastError(), "vision unfold");
}

__global__ static void v41_row_select_kernel(float *x, const float *src, const int32_t *rows, uint64_t n, uint32_t dim) {
    v41_pdl_wait();
    const uint64_t i = (uint64_t)blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    const int32_t r = rows[i / dim];
    if (r >= 0) x[i] = src[(uint64_t)r * dim + i % dim];
}
int ds4_gpu_v41_row_select_tensor(ds4_gpu_tensor *x, const ds4_gpu_tensor *src, const ds4_gpu_tensor *rows, uint32_t n_tok, uint32_t dim) {
    if (!x || !src || !rows || !n_tok || !dim) return 0;
    const uint64_t n = (uint64_t)n_tok * dim;
    v41_row_select_kernel<<<(unsigned)((n + 255) / 256), 256, 0, g_cur_stream>>>((float *)x->ptr, (const float *)src->ptr, (const int32_t *)rows->ptr, n, dim);
    return cuda_ok(cudaGetLastError(), "vision row select");
}

__global__ static void v41_rows_zero_kernel(float *x, const int32_t *rows, uint64_t n, uint32_t dim) {
    v41_pdl_wait();
    const uint64_t i = (uint64_t)blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    if (rows[i / dim] >= 0) x[i] = 0.0f;
}
int ds4_gpu_v41_rows_zero_tensor(ds4_gpu_tensor *x, const ds4_gpu_tensor *rows, uint32_t n_tok, uint32_t dim) {
    if (!x || !rows || !n_tok || !dim) return 0;
    const uint64_t n = (uint64_t)n_tok * dim;
    v41_rows_zero_kernel<<<(unsigned)((n + 255) / 256), 256, 0, g_cur_stream>>>((float *)x->ptr, (const int32_t *)rows->ptr, n, dim);
    return cuda_ok(cudaGetLastError(), "vision rows zero");
}
