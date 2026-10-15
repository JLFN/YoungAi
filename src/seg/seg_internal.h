/* seg_internal.h — seg_model.c / seg_encoder.c / seg_decoder.c / seg_run.c 之间的模型结构体: 权重已上传 GPU(seg_buf), 形状按 MobileSAM 固定。
 * 命名跟官方模块名走(docs/assistant_image.md §3.6 的结构表), 看着 tiny_vit_sam.py / mask_decoder.py 能一一对上。 */
#ifndef SEG_INTERNAL_H
#define SEG_INTERNAL_H
#include "ds4_seg.h"
#include "seg_ops.h"

typedef struct { seg_buf *w, *b; int out, in; } seg_lin;      /* Linear / 1×1 卷积 / im2col 后的 k×k 卷积: w [out][in], b [out](可 NULL) */
typedef struct { seg_buf *g, *b; float eps; int C; } seg_ln;    /* LayerNorm(eps 1e-5) 与 LayerNorm2d(eps 1e-6) */
typedef struct { seg_buf *w, *b; int C; } seg_dw;               /* 深度 3×3: w [C][9], b [C](BN 折进来的) */

typedef struct { seg_lin conv1, conv3; seg_dw conv2; } seg_mbconv;            /* 1×1 升 ×4 → dw3×3 → 1×1 降, 残差后 GELU */
typedef struct { seg_lin conv1, conv3; seg_dw conv2; int stride; } seg_merge;  /* PatchMerging: 1×1 → dw3×3(stride 2 或 1) → 1×1 */
typedef struct {
    seg_ln norm; seg_lin qkv, proj; seg_buf *bias;   /* bias: attention_biases 按窗口展开成 [H][N][N] */
    seg_dw local; seg_ln mlp_norm; seg_lin fc1, fc2;
} seg_tblock;
typedef struct { int dim, res, heads, win, depth; seg_tblock blocks[6]; seg_merge down; bool has_down; } seg_stage;

typedef struct { seg_lin q, k, v, o; int heads, inner; } seg_attn;   /* 解码器注意力: 256 → inner(256 或 128) → 256 */
typedef struct { seg_attn self_attn, cross_t2i, cross_i2t; seg_ln n1, n2, n3, n4; seg_lin mlp1, mlp2; } seg_twoway;
typedef struct { seg_lin l[3]; } seg_mlp3;

#define SEG_SCRATCH 12

struct seg_model {
    /* 编码器 TinyViT */
    seg_lin pe0, pe1;          /* patch_embed 两个 3×3 s2 卷积(im2col 布局 [out][ky][kx][cin]) */
    seg_mbconv mb[2]; seg_merge down0;
    seg_stage st[3];           /* layers 1..3 */
    seg_lin neck0, neck2; seg_ln neck1, neck3;
    /* 提示编码器(小向量留主机副本: token 表在主机拼好一次上传) */
    float gauss[2][128];       /* positional_encoding_gaussian_matrix */
    float pt_emb[4][256], nap[256], nomask[256], iou_tok[256], mask_tok[4][256];
    seg_buf *no_mask_dev, *image_pe;                            /* image_pe: [4096][256] 稠密位置编码 */
    seg_lin md0, md3, md6; seg_ln md1, md4;                     /* mask_downscaling: 2×2 s2 (1→4) → LN → GELU → 2×2 s2 (4→16) → LN → GELU → 1×1 (16→256) */
    /* 掩膜解码器 */
    seg_twoway layer[2]; seg_attn final_attn; seg_ln norm_final;
    seg_lin up0, up3; seg_ln up1;       /* ConvTranspose2d k2 s2 当 GEMM: w [Cout*4][Cin] */
    seg_mlp3 hyper[4], iou_head;
    /* 草稿区: 按下标取、不够就重分配(seg_scratch); 0..5 编码器, 6..11 解码器(编码器输出住 1, 解码两轮都要它) */
    seg_buf *scratch[SEG_SCRATCH]; size_t scratch_n[SEG_SCRATCH];
};

seg_buf *seg_scratch(seg_model *m, int i, size_t n);
/* 编码器: 输入 [1024][1024][3] 归一化图(设备) → 出 [4096][256] 嵌入(scratch 1) */
seg_buf *seg_encode(seg_model *m, seg_buf *img);
/* 解码器一次: emb = 编码器输出; pts 已换到 1024 帧坐标, labels 1/0; mask_in(可 NULL) = 低分辨率 logit [256][256](设备);
 * 出: *low = [4][65536] 低分辨率 logit(设备 scratch, 下次调用会被覆盖), iou[4] 主机 */
void seg_decode(seg_model *m, seg_buf *emb, const float *pts, const int *labels, int npts, const seg_buf *mask_in, size_t mask_off, seg_buf **low, float iou[4]);

#endif
