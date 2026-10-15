/* ds4_image.h — 改图工具的图像基元(方案稿 docs/assistant_image.md): 解码/PNG 编码、几何与调色、指点换色的掩膜与软 alpha。
 * 纯主机 C, 零外挂(解码 vendored stb_image, PNG 编码用 src/office 的 deflate)。像素 = RGB 8 位行主序; 坐标左上原点。
 * 谁用: src/assistant/tools/tool_image.c(edit_image 工具, 在沙盒子进程里跑)、src/seg(ds4-seg 的解码/PNG 写出)。 */
#ifndef DS4_IMAGE_H
#define DS4_IMAGE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { int w, h; uint8_t *rgb; } ds4_img;
typedef struct { int x, y, w, h; } img_rect;   /* w 或 h ≤ 0 = 整图 */

/* ---------------- 编解码(img_codec.c) ---------------- */
bool img_load(const char *path, ds4_img *im, char *err, size_t errn);                       /* jpg / png / bmp / gif 首帧 → RGB */
bool img_load_rgba(const char *path, int *w, int *h, uint8_t **rgba, char *err, size_t errn);   /* 贴图源: 带 alpha */
uint8_t *img_png_encode(int w, int h, int comps, const uint8_t *px, size_t *n);           /* comps 1 灰 / 3 RGB / 4 RGBA; malloc */
bool img_png_write(const char *path, int w, int h, int comps, const uint8_t *px);
void img_free(ds4_img *im);
uint32_t img_parse_color(const char *s, bool *ok);   /* "#rrggbb" / "#rgb" / 十几个英文颜色名 → 0xRRGGBB */

/* ---------------- 几何与调色(img_ops.c) ---------------- */
uint8_t *img_resize_ch(const uint8_t *in, int w, int h, int ch, int tw, int th);   /* PIL BILINEAR(缩小时三角核支撑按比例放大 = 抗锯齿), 任意通道数, malloc */
bool img_crop(ds4_img *im, img_rect r);
void img_resize(ds4_img *im, int tw, int th);
void img_rotate(ds4_img *im, int deg);          /* 90 / 180 / 270(顺时针) */
void img_flip(ds4_img *im, bool horizontal);
void img_adjust(ds4_img *im, img_rect r, float brightness, float contrast, float saturation);   /* 各 −100..100, 0 = 不动 */
void img_blur(ds4_img *im, img_rect r, int radius);
void img_pixelate(ds4_img *im, img_rect r, int block);
void img_draw_rect(ds4_img *im, img_rect r, uint32_t rgb, int width, bool fill);
void img_draw_ellipse(ds4_img *im, img_rect r, uint32_t rgb, int width, bool fill);
void img_draw_line(ds4_img *im, int x1, int y1, int x2, int y2, uint32_t rgb, int width, bool arrow);
void img_paste(ds4_img *im, const uint8_t *rgba, int sw, int sh, int x, int y, int tw, float opacity);   /* tw ≤ 0 = 原尺寸, 否则按宽等比缩 */
/* 放大核对图(新图): 原图 r 这块缩放到长边 out_long, 叠品红网格(线在原图坐标的整步长处, 交点标 "x,y" 原图像素坐标); outline 非空 = 裁剪框: 框外压暗、框画白线。r 全在图外 → rgb 为 NULL */
ds4_img img_zoom_grid(const ds4_img *im, img_rect r, int out_long, const img_rect *outline);

/* ---------------- 掩膜(img_mask.c): 颜色流水线(方案稿 §3.5 定稿) ---------------- */
/* pos/neg = (x,y) 对, 原图像素; 出 [w*h] 的 0/1 掩膜(malloc)。半分辨率算: 每个点一个高斯分量 + 边框一圈自动负样本 → min-NLL 竞争 → 补洞/连通/多数滤波 */
uint8_t *img_mask_color(const ds4_img *im, const int *pos, int npos, const int *neg, int nneg);

/* ---------------- 软 alpha 与换色(img_alpha.c) ---------------- */
float *img_alpha_guided(const ds4_img *im, const uint8_t *mask01, int radius, float eps);   /* 导向滤波(导向 = 原图灰度): 0/1 掩膜 → 贴真实边沿的软 alpha */
void img_recolor(ds4_img *im, const float *alpha, uint32_t rgb);         /* 目标色 + (像素亮度 − 掩膜平均亮度)×0.6, 按 alpha 混 */
ds4_img img_overlay(const ds4_img *im, const float *alpha, uint32_t rgb);   /* 叠色预览(新图): 原图·(1−0.55α) + 色·0.55α */
double img_alpha_share(const float *alpha, size_t n);
img_rect img_alpha_bbox(const float *alpha, int w, int h);
void img_boxblur_f(const float *in, int w, int h, int r, float *out);    /* (2r+1)² 均值, 边缘按最近像素延拓(积分图) */

#endif
