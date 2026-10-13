/* metal_v41_vision.m — 视觉塔原语在 Metal 侧的落点(2026-10-13)。契约 ds4_gpu_v41.h "视觉塔" 一节。
 *
 * ★现状 = 诚实的桩, 全部返回 0(失败)★。CUDA(src/cuda/cuda_v41_vision.inc.cu)是部署路; 16 GB 的 Mac 装不下 V4.1 Flash,
 * 这里没有真机可以过门, 所以不写"看起来能跑"的实现 —— 视觉塔算错不报错, 只会让模型看图胡说。
 * 行为: 带图片的请求在 Metal 上到 v41_vision_encode 第一发就失败, core 报"视觉塔在这个后端没有实现", 不会静默丢图。
 * 要补齐时: bias/rope/swiglu_pair/gelu/unfold/row_select/rows_zero 都是逐元素核(照 metal/v41_dense.metal 的写法),
 * dense_attn 可以借 metal/flash_attn_ext.metal 的无掩码路; 门同 CUDA: tools/vision_golden.py 的金标逐行 cos。 */
#import "metal_v41.h"

static int v41_vision_unimplemented(const char *what) {
    fprintf(stderr, "ds4: Metal 视觉塔原语 %s 还没有实现(CUDA 是部署路)\n", what);
    return 0;
}
int ds4_gpu_v41_bias_add_dev_tensor(ds4_gpu_tensor *x, const ds4_gpu_tensor *bias, uint32_t n_tok, uint32_t dim) {
    (void)x; (void)bias; (void)n_tok; (void)dim;
    return v41_vision_unimplemented("bias_add");
}
int ds4_gpu_v41_matmul_bf16_dev_tensor(ds4_gpu_tensor *out, const ds4_gpu_tensor *W, uint64_t in_dim, uint64_t out_dim, const ds4_gpu_tensor *x, uint32_t n_tok) {
    (void)out; (void)W; (void)in_dim; (void)out_dim; (void)x; (void)n_tok;
    return v41_vision_unimplemented("matmul_bf16_dev");
}
int ds4_gpu_v41_rms_norm_dev_tensor(ds4_gpu_tensor *out, const ds4_gpu_tensor *x, const ds4_gpu_tensor *w, uint32_t dim, uint32_t n_tok, float eps) {
    (void)out; (void)x; (void)w; (void)dim; (void)n_tok; (void)eps;
    return v41_vision_unimplemented("rms_norm_dev");
}
int ds4_gpu_v41_vision_rope_tensor(ds4_gpu_tensor *qkv, uint32_t n_tok, uint32_t n_w, uint32_t n_head, uint32_t head_dim, float theta) {
    (void)qkv; (void)n_tok; (void)n_w; (void)n_head; (void)head_dim; (void)theta;
    return v41_vision_unimplemented("vision_rope");
}
int ds4_gpu_v41_dense_attn_tensor(ds4_gpu_tensor *o, const ds4_gpu_tensor *qkv, uint32_t n_tok, uint32_t n_head, uint32_t head_dim) {
    (void)o; (void)qkv; (void)n_tok; (void)n_head; (void)head_dim;
    return v41_vision_unimplemented("dense_attn");
}
int ds4_gpu_v41_swiglu_pair_tensor(ds4_gpu_tensor *h, const ds4_gpu_tensor *gu, uint32_t n_tok, uint32_t mid) {
    (void)h; (void)gu; (void)n_tok; (void)mid;
    return v41_vision_unimplemented("swiglu_pair");
}
int ds4_gpu_v41_gelu_tensor(ds4_gpu_tensor *x, uint64_t n) {
    (void)x; (void)n;
    return v41_vision_unimplemented("gelu");
}
int ds4_gpu_v41_vision_unfold_tensor(ds4_gpu_tensor *out, const ds4_gpu_tensor *x, uint32_t n_h, uint32_t n_w, uint32_t dim, uint32_t r) {
    (void)out; (void)x; (void)n_h; (void)n_w; (void)dim; (void)r;
    return v41_vision_unimplemented("vision_unfold");
}
int ds4_gpu_v41_row_select_tensor(ds4_gpu_tensor *x, const ds4_gpu_tensor *src, const ds4_gpu_tensor *rows, uint32_t n_tok, uint32_t dim) {
    (void)x; (void)src; (void)rows; (void)n_tok; (void)dim;
    return v41_vision_unimplemented("row_select");
}
int ds4_gpu_v41_rows_zero_tensor(ds4_gpu_tensor *x, const ds4_gpu_tensor *rows, uint32_t n_tok, uint32_t dim) {
    (void)x; (void)rows; (void)n_tok; (void)dim;
    return v41_vision_unimplemented("rows_zero");
}
