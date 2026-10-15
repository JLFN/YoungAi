/* tool_image.h — 改图两个文件之间的内部约定: tool_image.c 做像素操作与结果句; tool_image_seg.c 做分割(找权重、主进程 spawn ds4-seg、
 * 点 → 范围 / 掩膜、抠图)。方案稿 docs/assistant_image.md §3.6~3.8。 */
#ifndef TOOL_IMAGE_H
#define TOOL_IMAGE_H
#include "../asst_internal.h"
#include "../../image/ds4_image.h"

#define MAX_PTS 32
#define SEG_PTS 8          /* 给分割模型的点最多各 8 个: 点一多 SAM 反而散(10-15 真模型门: 15 个点掩膜从 38% 塌到 21%) */
#define IMG_MAX_OPS 16     /* 一次调用里能各带一份分割的 op 数; 之后的 op 给了点就报错, 不静默忽略 */

/* 第 i 个 op 的分割结果。mask 空 = 没跑(没给点、在改画幅之后)或跑失败(why 写原因, 以 " (" 开头接在句子里) */
typedef struct { char mask[ASST_PATH + 48]; char why[200]; char dpos[256], dneg[256]; } seg_out;
typedef struct { const char *path, *out, *ops, *trash; char ws[ASST_PATH]; seg_out seg[IMG_MAX_OPS]; } image_job;

int img_parse_points(const char *raw, int *pts, int max);   /* "[[x,y],[x,y]]" 或 "[x,y,x,y]" → 成对的整数, 返回点数 */
void img_job_segment(image_job *j);                          /* 主进程: 改画幅之前每个带 points 的 op 跑一次 ds4-seg */
uint8_t *img_job_mask(const image_job *j, int i, const ds4_img *im);   /* 子进程: 第 i 个 op 的 0/1 掩膜(malloc); 没有 / 尺寸不符 → NULL */
void img_job_seg_notes(const image_job *j, int i, ds4_buf *b);        /* ds4-seg 丢掉的点 → 结果句 */
/* 带 points 的 op 的范围 = 分割出的东西的外接框, 每边外扩 grow × 框的宽/高。没给点 → *used = false 返回 true; 给了点但用不了 → 错误句进 b 返回 false */
bool img_points_region(const image_job *j, int i, const char *op, const char *name, const ds4_img *im, bool framed, float grow, img_rect *r, bool *used, ds4_buf *b);
/* cutout: 图裁到东西的外接框, 框里东西以外透明; *alpha = 0..255 的透明度平面(malloc, 与裁后的图同尺寸) */
bool img_cutout(const image_job *j, int i, const char *op, ds4_img *im, bool framed, uint8_t **alpha, ds4_buf *b);

#endif
