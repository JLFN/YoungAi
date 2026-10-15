/* img_grid.c — 放大核对图: 原图一块放大 + 标了原图像素坐标的网格(edit_image 每次改完回给模型看, docs/assistant_image.md §3.7)。
 * 为什么要它(10-15 探针, 恐龙椅 3072x4096 / 熊图 1920x1080): 模型指点的误差约是它看到的画面的 10%, 整图上一张 30 px 的脸怎么问都偏 40~90 px;
 * 同一块放大 3 倍再问, 偏 10~30 px; 放大且叠网格(交点标原图坐标)偏 7~13 px。整图上叠网格没用(答案被吸到网格数字上), 所以只给放大块画。
 * 字是自带的 5x7 点阵(只有数字、逗号、减号): 标签只写坐标, 不值得为它引字体库。 */
#include "ds4_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLYPH_S 3        /* 点阵放大倍数: 长边 1024 的核对图上字高 21 px, 视觉塔切 14 px 一块, 字小于一块模型读不准 */
#define GRID_RGB 0xff00ff  /* 品红: 照片里少见, 与红色改动(画圈、换红)也分得开 */

static const uint8_t FONT[13][7] = {   /* 每行 5 位, 高位在左; 顺序 0-9 , - 空格 */
    {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14}, {14, 17, 1, 2, 4, 8, 31}, {31, 2, 4, 2, 1, 17, 14}, {2, 6, 10, 18, 31, 2, 2},
    {31, 16, 30, 1, 1, 17, 14}, {6, 8, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8}, {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 2, 12},
    {0, 0, 0, 0, 12, 4, 8}, {0, 0, 0, 31, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0}};

static void blend(ds4_img *im, int x, int y, uint32_t rgb, float a) {
    if (x < 0 || y < 0 || x >= im->w || y >= im->h) return;
    uint8_t *p = im->rgb + ((size_t)y * im->w + x) * 3;
    for (int c = 0; c < 3; c++) p[c] = (uint8_t)(p[c] * (1 - a) + (float)((rgb >> (16 - 8 * c)) & 255) * a + 0.5f);
}
static void label(ds4_img *im, int x, int y, const char *s) {   /* (x,y) = 左下角 */
    const int n = (int)strlen(s), cw = 6 * GLYPH_S, h = 7 * GLYPH_S;
    for (int yy = y - h - 2; yy <= y + 1; yy++) for (int xx = x - 2; xx < x + n * cw + 1; xx++) blend(im, xx, yy, 0xffffff, 0.75f);
    for (int i = 0; i < n; i++) {
        const int g = s[i] >= '0' && s[i] <= '9' ? s[i] - '0' : s[i] == ',' ? 10 : s[i] == '-' ? 11 : 12;
        for (int r = 0; r < 7; r++) for (int c = 0; c < 5; c++) if (FONT[g][r] >> (4 - c) & 1)
            for (int dy = 0; dy < GLYPH_S; dy++) for (int dx = 0; dx < GLYPH_S; dx++) blend(im, x + i * cw + c * GLYPH_S + dx, y - h + r * GLYPH_S + dy, GRID_RGB, 1.0f);
    }
}

ds4_img img_zoom_grid(const ds4_img *im, img_rect r, int out_long, const img_rect *outline) {
    ds4_img z = {0};
    if (r.x < 0) { r.w += r.x; r.x = 0; } if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > im->w) r.w = im->w - r.x; if (r.y + r.h > im->h) r.h = im->h - r.y;
    if (r.w < 1 || r.h < 1) return z;
    uint8_t *cut = malloc((size_t)r.w * r.h * 3);
    for (int y = 0; y < r.h; y++) memcpy(cut + (size_t)y * r.w * 3, im->rgb + ((size_t)(r.y + y) * im->w + r.x) * 3, (size_t)r.w * 3);
    const double s = (double)out_long / (r.w > r.h ? r.w : r.h);
    z.w = (int)(r.w * s + 0.5); z.h = (int)(r.h * s + 0.5); if (z.w < 1) z.w = 1; if (z.h < 1) z.h = 1;
    z.rgb = img_resize_ch(cut, r.w, r.h, 3, z.w, z.h); free(cut);
    img_rect o = { 0, 0, 0, 0 };
    if (outline) {   /* 裁剪: 框外压暗到 35%, 亮着的就是留下的。只画一圈细框时模型看不出框只套住了水龙头左缘(10-15 真模型门) */
        o = (img_rect){ (int)((outline->x - r.x) * s), (int)((outline->y - r.y) * s), (int)(outline->w * s), (int)(outline->h * s) };
        for (int y = 0; y < z.h; y++) for (int x = 0; x < z.w; x++) if (x < o.x || y < o.y || x >= o.x + o.w || y >= o.y + o.h) blend(&z, x, y, 0x000000, 0.65f);
    }
    /* 网格步长取 1/2/5×10^k 里最小的、让相邻交点隔得下一个最长标签("12345,12345")的那个 */
    const double need = (11 * 6 * GLYPH_S + 20) / s;
    long step = 1;
    for (long m = 1; ; m *= 10) { if (m >= need) { step = m; break; } if (2 * m >= need) { step = 2 * m; break; } if (5 * m >= need) { step = 5 * m; break; } }
    const long gx0 = (r.x + step - 1) / step * step, gy0 = (r.y + step - 1) / step * step;
    for (long gx = gx0; gx < r.x + r.w; gx += step) { const int px = (int)((gx - r.x) * s); for (int y = 0; y < z.h; y++) { blend(&z, px, y, GRID_RGB, 0.7f); blend(&z, px + 1, y, GRID_RGB, 0.7f); } }
    for (long gy = gy0; gy < r.y + r.h; gy += step) { const int py = (int)((gy - r.y) * s); for (int x = 0; x < z.w; x++) { blend(&z, x, py, GRID_RGB, 0.7f); blend(&z, x, py + 1, GRID_RGB, 0.7f); } }
    if (outline) img_draw_rect(&z, o, 0xffffff, 3, false);
    char t[32];
    for (long gx = gx0; gx < r.x + r.w; gx += step) for (long gy = gy0; gy < r.y + r.h; gy += step) {
        snprintf(t, sizeof t, "%ld,%ld", gx, gy); label(&z, (int)((gx - r.x) * s) + 5, (int)((gy - r.y) * s) - 5, t);
    }
    return z;
}
