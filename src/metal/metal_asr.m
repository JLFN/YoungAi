/* metal_asr.m — 语音识别原语的 Metal 发射(2026-10-14)。契约 ds4_gpu_asr.h; 核在 metal/asr.metal(拼进库的最后一个文件, 见 metal_source.m)。
 * GEMM 借 v41_sgemm(f32 两张量相乘, 有转置位), 其余九件都是自己的小核。asr_args 是 metal/asr.metal 里同名 struct 的主机侧镜像。 */
#import "metal_v41.h"
#include "ds4_gpu_asr.h"

typedef struct { uint32_t n0, n1, n2, n3, n4, n5; float f0, f1; } asr_args;
static v41_scratch g_asr_stats;   /* groupnorm 的 mean / rstd 两个数 */

int ds4_gpu_asr_gemm_nt_tensor(ds4_gpu_tensor *out, const ds4_gpu_tensor *x, const ds4_gpu_tensor *W, uint32_t M, uint32_t N, uint32_t K) {
    return v41_sgemm(x, K, 0, W, K, 1, out, N, M, N, K, 1.0f, 0.0f, "asr gemm x·Wᵀ");
}
int ds4_gpu_asr_bias_add_tensor(ds4_gpu_tensor *x, const ds4_gpu_tensor *b, uint32_t n_rows, uint32_t dim) {
    if (!x || !b || !n_rows || !dim) return 0;
    asr_args a = { n_rows, dim, 0, 0, 0, 0, 0, 0 };
    v41_bind bd[] = { V41_A(a), V41_T(x), V41_T(b) };
    return v41_launch_1d("kernel_asr_bias_add", bd, 3, (uint64_t)n_rows * dim);
}
int ds4_gpu_asr_layernorm_tensor(ds4_gpu_tensor *out, const ds4_gpu_tensor *x, const ds4_gpu_tensor *w, uint32_t n_rows, uint32_t dim, float eps) {
    if (!out || !x || !w || !n_rows || !dim) return 0;
    asr_args a = { dim, 0, 0, 0, 0, 0, eps, 0 };
    v41_bind bd[] = { V41_A(a), V41_T(out), V41_T(x), V41_T(w) };
    return v41_launch("kernel_asr_layernorm", bd, 4, MTLSizeMake(n_rows, 1, 1), MTLSizeMake(256, 1, 1));
}
int ds4_gpu_asr_conv1d_tensor(ds4_gpu_tensor *out, const ds4_gpu_tensor *x, const ds4_gpu_tensor *w, const ds4_gpu_tensor *b,
                              uint32_t t_in, uint32_t c_in, uint32_t c_out, uint32_t k, uint32_t stride, uint32_t act) {
    if (!out || !x || !w || t_in < k || !stride || !c_in || !c_out) return 0;
    const uint32_t t_out = (t_in - k) / stride + 1u;
    asr_args a = { t_in, c_in, c_out, k, stride, act | (b ? 256u : 0u), 0, 0 };
    v41_bind bd[] = { V41_A(a), V41_T(out), V41_T(x), V41_T(w), V41_T(b ? b : w) };
    return v41_launch_1d("kernel_asr_conv1d", bd, 5, (uint64_t)t_out * c_out);
}
int ds4_gpu_asr_groupnorm1_tensor(ds4_gpu_tensor *x, const ds4_gpu_tensor *w, const ds4_gpu_tensor *b, uint32_t n_rows, uint32_t dim, float eps) {
    if (!x || !w || !b || !n_rows || !dim) return 0;
    id<MTLBuffer> st = v41_grow(&g_asr_stats, 64, "asr gn stats");
    if (!st) return 0;
    const uint64_t n = (uint64_t)n_rows * dim;
    asr_args a = { (uint32_t)n, dim, 0, 0, 0, 0, eps, 0 };
    v41_bind b1[] = { V41_A(a), V41_B(st, 0), V41_T(x) };
    if (!v41_launch("kernel_asr_gn_stats", b1, 3, MTLSizeMake(1, 1, 1), MTLSizeMake(1024, 1, 1))) return 0;
    v41_bind b2[] = { V41_A(a), V41_T(x), V41_T(w), V41_T(b), V41_B(st, 0) };
    return v41_launch_1d("kernel_asr_gn_apply", b2, 5, n);
}
int ds4_gpu_asr_gelu_tensor(ds4_gpu_tensor *x, uint64_t n) {
    if (!x || !n) return 0;
    asr_args a = { (uint32_t)n, 0, 0, 0, 0, 0, 0, 0 };
    v41_bind bd[] = { V41_A(a), V41_T(x) };
    return v41_launch_1d("kernel_asr_gelu", bd, 2, n);
}
int ds4_gpu_asr_rope_tensor(ds4_gpu_tensor *x, uint32_t n_tok, uint32_t n_head, uint32_t head_dim, uint32_t n_rot, float theta) {
    if (!x || !n_tok || !n_head || n_rot > head_dim || (n_rot & 1u)) return 0;
    asr_args a = { n_head, head_dim, n_rot, n_tok, 0, 0, theta, 0 };
    v41_bind bd[] = { V41_A(a), V41_T(x) };
    return v41_launch_1d("kernel_asr_rope", bd, 2, (uint64_t)n_tok * n_head * (n_rot / 2u));
}
int ds4_gpu_asr_attn_tensor(ds4_gpu_tensor *o, const ds4_gpu_tensor *q, const ds4_gpu_tensor *k, const ds4_gpu_tensor *v,
                            uint32_t nq, uint32_t nk, uint32_t n_head, uint32_t head_dim, int causal) {
    if (!o || !q || !k || !v || !nq || !nk || nk > 4096u || head_dim > 256u || (causal && nq > nk)) return 0;
    asr_args a = { nq, nk, n_head, head_dim, causal ? 1u : 0u, 0, 1.0f / sqrtf((float)head_dim), 0 };
    v41_bind bd[] = { V41_A(a), V41_T(o), V41_T(q), V41_T(k), V41_T(v) };
    return v41_launch("kernel_asr_attn", bd, 5, MTLSizeMake(nq, n_head, 1), MTLSizeMake(256, 1, 1));
}
int ds4_gpu_asr_swiglu_tensor(ds4_gpu_tensor *h, const ds4_gpu_tensor *gu, uint32_t n_rows, uint32_t mid) {
    if (!h || !gu || !n_rows || !mid) return 0;
    const uint64_t n = (uint64_t)n_rows * mid;
    asr_args a = { (uint32_t)n, mid, 0, 0, 0, 0, 0, 0 };
    v41_bind bd[] = { V41_A(a), V41_T(h), V41_T(gu) };
    return v41_launch_1d("kernel_asr_swiglu", bd, 3, n);
}
