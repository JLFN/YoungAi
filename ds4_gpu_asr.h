/* ds4_gpu_asr.h — 语音识别(Moonshine 族 encoder-decoder, src/model/asr/)的 GPU 原语契约(2026-10-14)。
 * Metal 实现 src/metal/metal_asr.m + metal/asr.metal; CUDA 实现 src/cuda/cuda_asr.inc.cu。
 *
 * 口径: 全部 f32、行主序、不舍 bf16 —— 模型本身是 f32 小模型(27M), 门是对 HF 官方 ONNX 参考实现的逐步 logits 逐位比
 * (tools/asr_golden.py), 掺任何 bf16 舍入都会把门变成"差不多"。为什么不借 V4.1 的同名核: 那一族在模块边界按官方 bf16 纪律舍,
 * 而且 RoPE 是相邻配对转末尾维(HF Moonshine 是 rotate_half 转开头维), 视觉塔那族在 Metal 上还是桩。
 * 张量都是 ds4_gpu_tensor_alloc 来的设备张量(权重来音频时才拷上去, 跑完释放, 与视觉塔同规矩), 不走模型映射。
 * 出错会怎样: 任何一件算错不报错 —— 识别出来是别的字; 所以 ds4_test --asr 的逐位门是唯一的正确性依据。 */
#ifndef DS4_GPU_ASR_H
#define DS4_GPU_ASR_H
#include "ds4_gpu_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* out[M][N] = x[M][K] · W[N][K]ᵀ (nn.Linear 的权重存法, 不转置) */
int ds4_gpu_asr_gemm_nt_tensor(ds4_gpu_tensor *out, const ds4_gpu_tensor *x, const ds4_gpu_tensor *W, uint32_t M, uint32_t N, uint32_t K);
/* x[n_rows][dim] += b[dim] */
int ds4_gpu_asr_bias_add_tensor(ds4_gpu_tensor *x, const ds4_gpu_tensor *b, uint32_t n_rows, uint32_t dim);
/* out[r] = (x[r] − mean) / sqrt(var + eps) · w (LayerNorm 无偏置, 官方 nn.LayerNorm(bias=False); 方差按有偏估计) */
int ds4_gpu_asr_layernorm_tensor(ds4_gpu_tensor *out, const ds4_gpu_tensor *x, const ds4_gpu_tensor *w, uint32_t n_rows, uint32_t dim, float eps);
/* 一维卷积(无填充): x[t_in][c_in] 时间主序, w[c_out][c_in][k](torch Conv1d 存法), b 可空;
 * out[t][co] = act(b[co] + Σ_ci Σ_j w[co][ci][j] · x[t·stride + j][ci]), t < (t_in − k) / stride + 1; act: 0 无 / 1 tanh / 2 gelu(erf 精确版) */
int ds4_gpu_asr_conv1d_tensor(ds4_gpu_tensor *out, const ds4_gpu_tensor *x, const ds4_gpu_tensor *w, const ds4_gpu_tensor *b,
                              uint32_t t_in, uint32_t c_in, uint32_t c_out, uint32_t k, uint32_t stride, uint32_t act);
/* GroupNorm(num_groups=1) 就地: 均值/方差是整段 x[n_rows][dim] 一起算一个(不是逐通道), 再按通道 w[dim]·x + b[dim] */
int ds4_gpu_asr_groupnorm1_tensor(ds4_gpu_tensor *x, const ds4_gpu_tensor *w, const ds4_gpu_tensor *b, uint32_t n_rows, uint32_t dim, float eps);
/* 就地 x = gelu(x), erf 精确版(官方 F.gelu 默认) */
int ds4_gpu_asr_gelu_tensor(ds4_gpu_tensor *x, uint64_t n);
/* RoPE(HF Moonshine 式 = 相邻配对): x[n_tok][n_head][head_dim], 位置 = 行号; 每个头只转前 n_rot 维, 第 2i 维配第 2i+1 维,
 * 角 = pos · theta^(−2i/n_rot); 后 head_dim − n_rot 维不动。q、k 各调一次。
 * ★不是 Llama 的 rotate_half(前后半分)★: 10-14 用 numpy 参考对 ONNX 终输出判定, 相邻配对差 2.6e-5, 半分式差 9.39。 */
int ds4_gpu_asr_rope_tensor(ds4_gpu_tensor *x, uint32_t n_tok, uint32_t n_head, uint32_t head_dim, uint32_t n_rot, float theta);
/* 稠密注意力(官方 F.scaled_dot_product_attention, scale = head_dim^-½): q[nq][H·D], k/v[nk][H·D] → o[nq][H·D]。
 * causal=1 时只看 j ≤ i + (nk − nq)(解码器自注意力; nq == nk 就是标准因果); 0 = 全看(编码器 / 交叉注意力)。nk ≤ 4096。 */
int ds4_gpu_asr_attn_tensor(ds4_gpu_tensor *o, const ds4_gpu_tensor *q, const ds4_gpu_tensor *k, const ds4_gpu_tensor *v,
                            uint32_t nq, uint32_t nk, uint32_t n_head, uint32_t head_dim, int causal);
/* 解码器 MLP 的门: gu[n][2·mid] ★前半 up、后半 gate★(官方 MoonshineDecoderMLP: hidden, gate = fc1(x).chunk(2); act(gate)·hidden); h[n][mid] = silu(gate) · up。
 * 10-14 numpy 参考对 ONNX 判定: 这个顺序差 2.4e-5, 反过来差 25; 交叉注意力的 q/k 不转 RoPE(转了差 20)。 */
int ds4_gpu_asr_swiglu_tensor(ds4_gpu_tensor *h, const ds4_gpu_tensor *gu, uint32_t n_rows, uint32_t mid);

#ifdef __cplusplus
}
#endif
#endif /* DS4_GPU_ASR_H */
