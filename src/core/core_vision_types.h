/* core_vision_types.h — 视觉塔(DeepSeek-ViT)+ aligner 的权重句柄(2026-10-13), 只被 core_types.h 包含。
 * 张量名是转换器(gguf-tools/quantize/v41_to_gguf_vision.inc.c)写的 v.* / mm.*; 绑定与形状校验在 core_bind_vision.c。
 * 全部是 BF16 原件(矩阵)与 f32(norm/bias/三个学到的定界向量) —— 视觉塔只有 0.3B 参数, 与三塔同理不跟主干一起压。 */
#ifndef DS4_CORE_VISION_TYPES_H
#define DS4_CORE_VISION_TYPES_H

#define DS4_VIS_MAX_LAYERS 64   /* 官方 32; 真值读 GGUF deepseek4.vision.block_count */

typedef struct {
    ds4_tensor *norm1, *qkv_w, *qkv_b, *o_w, *o_b, *norm2, *up_w, *down_w;   /* up_w = 官方 w1(gate|up 并排 2·inter 行), down_w = w2 */
} ds4_vision_block_weights;

typedef struct {
    ds4_tensor *patch_w, *patch_b;                   /* patch_embed.proj: [3·p·p → dim] + bias */
    ds4_vision_block_weights blk[DS4_VIS_MAX_LAYERS];
    ds4_tensor *out_norm;                            /* vision.norm */
    ds4_tensor *mm0_w, *mm0_b, *mm1_w, *mm1_b;       /* aligner.w1 [dim·r² → E] / aligner.w2 [E → E] */
    ds4_tensor *image_start, *image_end, *image_newline;   /* 三个学到的定界向量 [E], 盖在 span 的对应位置上 */
} ds4_vision_weights;

#endif
