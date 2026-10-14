/* cuda_asr.inc.cu — ds4_cuda.cu 分片: 语音识别(Moonshine 族)原语(2026-10-14)。契约 ds4_gpu_asr.h, Metal 对应 metal/asr.metal。
 * 口径: 全 f32、不舍 bf16(门是对 ONNX 参考的逐位比, 见 tools/asr_golden.py)。形状都很小(d=288, 帧 ≤ 1250), 核只求写对、好读;
 * GEMM 走 cuBLAS Sgemm, 其余是一线程一元素 / 一块一行的朴素核, 与 Metal 版逐句对应(累加序相同 ⇒ 两后端结果只差 fma 合并)。
 * 借 cuda_v41_1 的 v41_scratch / v41_grow 与 cuda_internal 的 cuda_ok / cublas_ok / v41_pdl_wait。 */

static v41_scratch g_asr_stats;   /* groupnorm 的 mean / rstd 两个数 */

__device__ __forceinline__ static float asr_gelu(float x) { return 0.5f * x * (1.0f + erff(x * 0.70710678118654752f)); }
/* 一个 block(blockDim 线程)的求和 / 求最大: warp 内 shuffle, 再经 shared 汇总; 每个线程都得到同一个结果 */
__device__ static float asr_block_sum(float v, float *sh) {
    for (int o = 16; o > 0; o >>= 1) v += __shfl_xor_sync(0xffffffffu, v, o);
    __syncthreads();
    if ((threadIdx.x & 31u) == 0u) sh[threadIdx.x >> 5] = v;
    __syncthreads();
    float t = 0.0f;
    for (unsigned i = 0; i < (blockDim.x >> 5); i++) t += sh[i];
    return t;
}
__device__ static float asr_block_max(float v, float *sh) {
    for (int o = 16; o > 0; o >>= 1) v = fmaxf(v, __shfl_xor_sync(0xffffffffu, v, o));
    __syncthreads();
    if ((threadIdx.x & 31u) == 0u) sh[threadIdx.x >> 5] = v;
    __syncthreads();
    float t = sh[0];
    for (unsigned i = 1; i < (blockDim.x >> 5); i++) t = fmaxf(t, sh[i]);
    return t;
}

int ds4_gpu_asr_gemm_nt_tensor(ds4_gpu_tensor *out, const ds4_gpu_tensor *x, const ds4_gpu_tensor *W, uint32_t M, uint32_t N, uint32_t K) {
    if (!out || !x || !W || !g_cublas_ready || !M || !N || !K) return 0;
    /* 行主序 out[M][N] = x·Wᵀ 在列主序眼里是 outᵀ(N×M) = W(N×K, 即盘上 [N][K] 的转置视图) · xᵀ(K×M) */
    const float alpha = 1.0f, beta = 0.0f;
    (void)cublasSetStream(g_cublas, v41_cublas_stream());
    /* 句柄默认开着 TF32(cuda_lifecycle.inc.cu, 给主干的 bf16 路用): 10 位尾数把 f32 GEMM 压到 5e-4 相对误差, 10-14 spark 首跑编码器
     * max|Δ| 2.9e-3(Metal 4e-5)就是它。这里要 f32 逐位门, 临时切回纯 f32, 做完复原 —— 不动别人的模式 */
    cublasMath_t saved = CUBLAS_DEFAULT_MATH;
    (void)cublasGetMathMode(g_cublas, &saved);
    (void)cublasSetMathMode(g_cublas, CUBLAS_DEFAULT_MATH);
    cublasStatus_t st = cublasSgemm(g_cublas, CUBLAS_OP_T, CUBLAS_OP_N, (int)N, (int)M, (int)K, &alpha,
                                    (const float *)W->ptr, (int)K, (const float *)x->ptr, (int)K, &beta, (float *)out->ptr, (int)N);
    (void)cublasSetMathMode(g_cublas, saved);
    return cublas_ok(st, "asr gemm x·Wᵀ");
}

__global__ static void asr_bias_add_kernel(float *x, const float *b, uint64_t n, uint32_t dim) {
    v41_pdl_wait();
    const uint64_t i = (uint64_t)blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) x[i] += b[i % dim];
}
int ds4_gpu_asr_bias_add_tensor(ds4_gpu_tensor *x, const ds4_gpu_tensor *b, uint32_t n_rows, uint32_t dim) {
    if (!x || !b || !n_rows || !dim) return 0;
    const uint64_t n = (uint64_t)n_rows * dim;
    asr_bias_add_kernel<<<(unsigned)((n + 255) / 256), 256, 0, g_cur_stream>>>((float *)x->ptr, (const float *)b->ptr, n, dim);
    return cuda_ok(cudaGetLastError(), "asr bias add");
}

/* 一 block 一行: 两遍(先均值再离差平方), 与 torch LayerNorm 同口径 */
__global__ static void asr_layernorm_kernel(float *out, const float *x, const float *w, uint32_t dim, float eps) {
    __shared__ float sh[32];
    v41_pdl_wait();
    const float *xr = x + (uint64_t)blockIdx.x * dim;
    float s = 0.0f;
    for (uint32_t i = threadIdx.x; i < dim; i += blockDim.x) s += xr[i];
    const float mean = asr_block_sum(s, sh) / (float)dim;
    float v = 0.0f;
    for (uint32_t i = threadIdx.x; i < dim; i += blockDim.x) { const float d = xr[i] - mean; v += d * d; }
    const float rstd = rsqrtf(asr_block_sum(v, sh) / (float)dim + eps);
    float *o = out + (uint64_t)blockIdx.x * dim;
    for (uint32_t i = threadIdx.x; i < dim; i += blockDim.x) o[i] = (xr[i] - mean) * rstd * w[i];
}
int ds4_gpu_asr_layernorm_tensor(ds4_gpu_tensor *out, const ds4_gpu_tensor *x, const ds4_gpu_tensor *w, uint32_t n_rows, uint32_t dim, float eps) {
    if (!out || !x || !w || !n_rows || !dim) return 0;
    asr_layernorm_kernel<<<n_rows, 256, 0, g_cur_stream>>>((float *)out->ptr, (const float *)x->ptr, (const float *)w->ptr, dim, eps);
    return cuda_ok(cudaGetLastError(), "asr layernorm");
}

/* 一线程一个输出 (t, co); 与 Metal 版同一累加序(ci 外 j 内) */
__global__ static void asr_conv1d_kernel(float *out, const float *x, const float *w, const float *b, uint32_t t_in, uint32_t c_in, uint32_t c_out,
                                         uint32_t k, uint32_t stride, uint32_t act) {
    v41_pdl_wait();
    const uint32_t t_out = (t_in - k) / stride + 1u;
    const uint64_t gid = (uint64_t)blockIdx.x * blockDim.x + threadIdx.x;
    if (gid >= (uint64_t)t_out * c_out) return;
    const uint32_t t = (uint32_t)(gid / c_out), co = (uint32_t)(gid % c_out);
    const float *wc = w + (uint64_t)co * c_in * k;
    const float *xt = x + (uint64_t)t * stride * c_in;
    float acc = b ? b[co] : 0.0f;
    for (uint32_t ci = 0; ci < c_in; ci++)
        for (uint32_t j = 0; j < k; j++) acc += wc[ci * k + j] * xt[(uint64_t)j * c_in + ci];
    if (act == 1u) acc = tanhf(acc);
    else if (act == 2u) acc = asr_gelu(acc);
    out[gid] = acc;
}
int ds4_gpu_asr_conv1d_tensor(ds4_gpu_tensor *out, const ds4_gpu_tensor *x, const ds4_gpu_tensor *w, const ds4_gpu_tensor *b,
                              uint32_t t_in, uint32_t c_in, uint32_t c_out, uint32_t k, uint32_t stride, uint32_t act) {
    if (!out || !x || !w || t_in < k || !stride || !c_in || !c_out) return 0;
    const uint64_t n = (uint64_t)((t_in - k) / stride + 1u) * c_out;
    asr_conv1d_kernel<<<(unsigned)((n + 255) / 256), 256, 0, g_cur_stream>>>((float *)out->ptr, (const float *)x->ptr, (const float *)w->ptr,
                                                                            b ? (const float *)b->ptr : NULL, t_in, c_in, c_out, k, stride, act);
    return cuda_ok(cudaGetLastError(), "asr conv1d");
}

/* GroupNorm(1 组): 单 block 1024 线程对整段两遍求 mean / rstd, 再一发按通道仿射 */
__global__ static void asr_gn_stats_kernel(float *stats, const float *x, uint32_t n, float eps) {
    __shared__ float sh[32];
    v41_pdl_wait();
    float s = 0.0f;
    for (uint32_t i = threadIdx.x; i < n; i += blockDim.x) s += x[i];
    const float mean = asr_block_sum(s, sh) / (float)n;
    __syncthreads();
    float v = 0.0f;
    for (uint32_t i = threadIdx.x; i < n; i += blockDim.x) { const float d = x[i] - mean; v += d * d; }
    const float var = asr_block_sum(v, sh) / (float)n;
    if (threadIdx.x == 0) { stats[0] = mean; stats[1] = rsqrtf(var + eps); }
}
__global__ static void asr_gn_apply_kernel(float *x, const float *w, const float *b, const float *stats, uint32_t n, uint32_t dim) {
    v41_pdl_wait();
    const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    const uint32_t c = i % dim;
    x[i] = (x[i] - stats[0]) * stats[1] * w[c] + b[c];
}
int ds4_gpu_asr_groupnorm1_tensor(ds4_gpu_tensor *x, const ds4_gpu_tensor *w, const ds4_gpu_tensor *b, uint32_t n_rows, uint32_t dim, float eps) {
    if (!x || !w || !b || !n_rows || !dim) return 0;
    float *st = (float *)v41_grow(&g_asr_stats, 64, "asr gn stats");
    if (!st) return 0;
    const uint32_t n = n_rows * dim;
    asr_gn_stats_kernel<<<1, 1024, 0, g_cur_stream>>>(st, (const float *)x->ptr, n, eps);
    if (!cuda_ok(cudaGetLastError(), "asr groupnorm stats")) return 0;
    asr_gn_apply_kernel<<<(n + 255) / 256, 256, 0, g_cur_stream>>>((float *)x->ptr, (const float *)w->ptr, (const float *)b->ptr, st, n, dim);
    return cuda_ok(cudaGetLastError(), "asr groupnorm apply");
}

__global__ static void asr_gelu_kernel(float *x, uint64_t n) {
    v41_pdl_wait();
    const uint64_t i = (uint64_t)blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) x[i] = asr_gelu(x[i]);
}
int ds4_gpu_asr_gelu_tensor(ds4_gpu_tensor *x, uint64_t n) {
    if (!x || !n) return 0;
    asr_gelu_kernel<<<(unsigned)((n + 255) / 256), 256, 0, g_cur_stream>>>((float *)x->ptr, n);
    return cuda_ok(cudaGetLastError(), "asr gelu");
}

/* 相邻配对 RoPE((2i, 2i+1) 一对, 见 metal/asr.metal 同名核的注释): 一线程一组 (t, h, i < n_rot/2) */
__global__ static void asr_rope_kernel(float *x, uint32_t n_tok, uint32_t n_head, uint32_t head_dim, uint32_t n_rot, float theta) {
    v41_pdl_wait();
    const uint32_t half_rot = n_rot / 2u, gid = blockIdx.x * blockDim.x + threadIdx.x;
    if (gid >= n_tok * n_head * half_rot) return;
    const uint32_t i = gid % half_rot, h = (gid / half_rot) % n_head, t = gid / (half_rot * n_head);
    float *p = x + ((uint64_t)t * n_head + h) * head_dim + 2u * i;
    /* 角度用 double 算: nvcc --use_fast_math 把 sinf/cosf 换成 __sinf/__cosf, 它们对大自变量(帧 140 × 频率 1 ≈ 140 rad)的误差随自变量放大,
     * 10-14 spark 首跑编码器 max|Δ| 2.6e-3(Metal 4e-5)就是它; double 的 sin/cos 不受 fast-math 影响, 这里一共才几万个角, 不计成本 */
    const double ang = (double)t * pow((double)theta, -(double)(2u * i) / (double)n_rot);
    const float c = (float)cos(ang), s = (float)sin(ang), x1 = p[0], x2 = p[1];
    p[0] = x1 * c - x2 * s;
    p[1] = x2 * c + x1 * s;
}
int ds4_gpu_asr_rope_tensor(ds4_gpu_tensor *x, uint32_t n_tok, uint32_t n_head, uint32_t head_dim, uint32_t n_rot, float theta) {
    if (!x || !n_tok || !n_head || n_rot > head_dim || (n_rot & 1u)) return 0;
    const uint32_t n = n_tok * n_head * (n_rot / 2u);
    asr_rope_kernel<<<(n + 255) / 256, 256, 0, g_cur_stream>>>((float *)x->ptr, n_tok, n_head, head_dim, n_rot, theta);
    return cuda_ok(cudaGetLastError(), "asr rope");
}

/* 稠密注意力: 一 block 一个 (查询 i, 头 h), 分数进 shared(nk ≤ 4096), softmax 后 D 个线程各算一维输出 */
__global__ static void asr_attn_kernel(float *o, const float *q, const float *k, const float *v, uint32_t nq, uint32_t nk, uint32_t H, uint32_t D,
                                       uint32_t causal, float scale) {
    __shared__ float sc[4096];
    __shared__ float sh[32];
    v41_pdl_wait();
    const uint32_t i = blockIdx.x, h = blockIdx.y, W = H * D;
    const uint32_t jmax = causal ? i + (nk - nq) : nk - 1u;
    const float *qi = q + (uint64_t)i * W + h * D;
    float mx = -1e30f;
    for (uint32_t j = threadIdx.x; j < nk; j += blockDim.x) {
        float s = -1e30f;
        if (j <= jmax) {
            const float *kj = k + (uint64_t)j * W + h * D;
            s = 0.0f;
            for (uint32_t d = 0; d < D; d++) s += qi[d] * kj[d];
            s *= scale;
        }
        sc[j] = s;
        mx = fmaxf(mx, s);
    }
    mx = asr_block_max(mx, sh);
    __syncthreads();
    float sum = 0.0f;
    for (uint32_t j = threadIdx.x; j < nk; j += blockDim.x) { const float p = j <= jmax ? expf(sc[j] - mx) : 0.0f; sc[j] = p; sum += p; }
    sum = asr_block_sum(sum, sh);
    __syncthreads();
    if (threadIdx.x < D) {
        float acc = 0.0f;
        for (uint32_t j = 0; j <= jmax; j++) acc += sc[j] * v[(uint64_t)j * W + h * D + threadIdx.x];
        o[(uint64_t)i * W + h * D + threadIdx.x] = acc / sum;
    }
}
int ds4_gpu_asr_attn_tensor(ds4_gpu_tensor *o, const ds4_gpu_tensor *q, const ds4_gpu_tensor *k, const ds4_gpu_tensor *v,
                            uint32_t nq, uint32_t nk, uint32_t n_head, uint32_t head_dim, int causal) {
    if (!o || !q || !k || !v || !nq || !nk || nk > 4096u || head_dim > 256u || (causal && nq > nk)) return 0;
    asr_attn_kernel<<<dim3(nq, n_head), 256, 0, g_cur_stream>>>((float *)o->ptr, (const float *)q->ptr, (const float *)k->ptr, (const float *)v->ptr,
                                                               nq, nk, n_head, head_dim, causal ? 1u : 0u, 1.0f / sqrtf((float)head_dim));
    return cuda_ok(cudaGetLastError(), "asr attn");
}

__global__ static void asr_swiglu_kernel(float *h, const float *gu, uint64_t n, uint32_t mid) {
    v41_pdl_wait();
    const uint64_t i = (uint64_t)blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    const uint64_t r = i / mid, c = i % mid;
    const float u = gu[r * 2u * mid + c], g = gu[r * 2u * mid + mid + c];   /* 前半 up、后半 gate(见 metal/asr.metal 同名核) */
    h[i] = g / (1.0f + expf(-g)) * u;
}
int ds4_gpu_asr_swiglu_tensor(ds4_gpu_tensor *h, const ds4_gpu_tensor *gu, uint32_t n_rows, uint32_t mid) {
    if (!h || !gu || !n_rows || !mid) return 0;
    const uint64_t n = (uint64_t)n_rows * mid;
    asr_swiglu_kernel<<<(unsigned)((n + 255) / 256), 256, 0, g_cur_stream>>>((float *)h->ptr, (const float *)gu->ptr, n, mid);
    return cuda_ok(cudaGetLastError(), "asr swiglu");
}
