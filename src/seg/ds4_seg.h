/* ds4_seg.h — 点提示分割(MobileSAM, 方案稿 docs/assistant_image.md §3.6)的公共接口。
 *
 * 用法: seg_pt_open(权重 .pt) → seg_model_load(张量表) → seg_run(图 + 点) → 掩膜; 全部纯 C, 权重直接读官方 .pt(zip + pickle),
 * 不转格式、不要 torch。计算走 GPU(seg_ops.h: Metal / CUDA 两份实现), 主机只做预处理(缩放/归一化/补边)和后处理(掩膜放回原图尺寸)。
 * 生命周期 = 一个进程(ds4-seg): 跑完退出, 权重和 GPU 上下文整个交还系统("用的时候加载, 用完释放", 10-15 用户定)。 */
#ifndef DS4_SEG_H
#define DS4_SEG_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------- 权重文件(seg_pt.c): torch.save 的 zip(stored 条目) + data.pkl(pickle 协议 2 的 state_dict) ---------------- */
typedef struct {
    char name[128];
    int ndim; int shape[6];
    size_t numel;
    const float *data;   /* 指向映射的 zip 条目(stored)或解压缓冲(deflated); 行主序连续 */
} seg_tensor;

typedef struct seg_pt seg_pt;
seg_pt *seg_pt_open(const char *path, char *err, size_t errn);   /* 只收 FloatStorage 张量; LongStorage(num_batches_tracked)跳过 */
int seg_pt_count(const seg_pt *p);
const seg_tensor *seg_pt_at(const seg_pt *p, int i);
const seg_tensor *seg_pt_find(const seg_pt *p, const char *name);   /* 没有 → NULL */
void seg_pt_close(seg_pt *p);

/* ---------------- 模型(seg_model.c / seg_encoder.c / seg_decoder.c) ---------------- */
#define SEG_IMG 1024          /* 编码器输入边长(官方 img_size) */
#define SEG_EMB 64            /* 图像嵌入边长(1024/16) */
#define SEG_DIM 256           /* 嵌入/提示维度 */
#define SEG_LOWRES 256        /* 解码器低分辨率掩膜边长 */
#define SEG_MAX_POINTS 32

typedef struct seg_model seg_model;
seg_model *seg_model_load(const seg_pt *pt, char *err, size_t errn);   /* BN 折进卷积、权重重排并上传 GPU; 失败 NULL */
void seg_model_free(seg_model *m);

/* 一次分割: rgb = 原图 HWC 8 位; pts = 原图像素坐标 (x,y) 对, label 1 = 要的 / 0 = 不要的;
 * 出: mask = 原图尺寸的掩膜 logit(>0 为内; malloc, 调用方 free), iou = 模型自报的掩膜质量(0..1)。
 * 规矩照官方 predictor: ≥2 个点单掩膜输出; 1 个点三掩膜取 IoU 最高; 之后把低分辨率 logit 当 mask 提示再跑一轮解码器(refine)。 */
bool seg_run(seg_model *m, const uint8_t *rgb, int w, int h, const float *pts, const int *label, int npts, float **mask, float *iou, int *kept, char *err, size_t errn);   /* kept[npts](可 NULL): 每个点有没有被用上(seg_run.c filter_points: 贴边负点、落在掩膜外的正点被丢) */

/* ---------------- 主机侧图像基元(seg_image.c) ---------------- */
void seg_preprocess_shape(int w, int h, int *tw, int *th);   /* 长边缩到 1024, 另一边四舍五入(官方 get_preprocess_shape) */
float *seg_resize_f32(const float *in, int w, int h, int tw, int th);        /* torch F.interpolate bilinear(align_corners=False, 无抗锯齿), 单通道, malloc */

#endif
