/* img_alpha.c — 0/1 掩膜 → 软 alpha(导向滤波, He et al. 2010, 导向 = 原图灰度)、换色、叠色预览、占比/包围框。
 * 为什么导向滤波: 掩膜在半分辨率或 256² 上算出来, 直接放大边沿全是台阶; 以原图为导向, alpha 的梯度跟着真实边沿走, 放大图里看不到锯齿(10-15 探针)。
 * 换色保留明暗: 目标色 + (像素亮度 − 掩膜平均亮度)×0.6 —— 纯黑目标也留着褶皱和罗纹, 不糊成一块。 */
#include "ds4_image.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* (2r+1)² 均值, 边缘按最近像素延拓: 先把图延拓 r 圈再做积分图(double, 1284×1718 不溢精度) */
void img_boxblur_f(const float *in, int w, int h, int r, float *out) {
    const int W = w + 2 * r, H = h + 2 * r; double *sat = calloc((size_t)(W + 1) * (H + 1), sizeof(double));
    for (int y = 0; y < H; y++) {
        const int sy = y - r < 0 ? 0 : y - r >= h ? h - 1 : y - r;
        for (int x = 0; x < W; x++) { const int sx = x - r < 0 ? 0 : x - r >= w ? w - 1 : x - r;
            sat[(size_t)(y + 1) * (W + 1) + x + 1] = in[(size_t)sy * w + sx] + sat[(size_t)y * (W + 1) + x + 1] + sat[(size_t)(y + 1) * (W + 1) + x] - sat[(size_t)y * (W + 1) + x]; }
    }
    const double area = (double)(2 * r + 1) * (2 * r + 1);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        const int y0 = y, x0 = x, y1 = y + 2 * r + 1, x1 = x + 2 * r + 1;
        out[(size_t)y * w + x] = (float)((sat[(size_t)y1 * (W + 1) + x1] - sat[(size_t)y0 * (W + 1) + x1] - sat[(size_t)y1 * (W + 1) + x0] + sat[(size_t)y0 * (W + 1) + x0]) / area);
    }
    free(sat);
}

float *img_alpha_guided(const ds4_img *im, const uint8_t *mask01, int radius, float eps) {
    const int w = im->w, h = im->h; const size_t n = (size_t)w * h;
    float *I = malloc(n * 4), *p = malloc(n * 4), *mI = malloc(n * 4), *mp = malloc(n * 4), *mIp = malloc(n * 4), *mII = malloc(n * 4), *A = malloc(n * 4), *B = malloc(n * 4), *t = malloc(n * 4);
    for (size_t i = 0; i < n; i++) { I[i] = (im->rgb[i * 3] + im->rgb[i * 3 + 1] + im->rgb[i * 3 + 2]) / (3.0f * 255.0f); p[i] = mask01[i] ? 1.0f : 0.0f; }
    img_boxblur_f(I, w, h, radius, mI); img_boxblur_f(p, w, h, radius, mp);
    for (size_t i = 0; i < n; i++) t[i] = I[i] * p[i]; img_boxblur_f(t, w, h, radius, mIp);
    for (size_t i = 0; i < n; i++) t[i] = I[i] * I[i]; img_boxblur_f(t, w, h, radius, mII);
    for (size_t i = 0; i < n; i++) { A[i] = (mIp[i] - mI[i] * mp[i]) / (mII[i] - mI[i] * mI[i] + eps); B[i] = mp[i] - A[i] * mI[i]; }
    img_boxblur_f(A, w, h, radius, mI); img_boxblur_f(B, w, h, radius, mp);   /* 复用 mI/mp 装 ā/b̄ */
    float *alpha = malloc(n * 4);
    for (size_t i = 0; i < n; i++) { float v = mI[i] * I[i] + mp[i]; alpha[i] = v < 0 ? 0 : v > 1 ? 1 : v; }
    free(I); free(p); free(mI); free(mp); free(mIp); free(mII); free(A); free(B); free(t);
    return alpha;
}

static uint8_t clip8(double v) { v = floor(v + 0.5); return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

void img_recolor(ds4_img *im, const float *alpha, uint32_t rgb) {
    const size_t n = (size_t)im->w * im->h; const double tc[3] = { (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255 };
    double sumL = 0, sumA = 0;
    for (size_t i = 0; i < n; i++) { const double L = (im->rgb[i * 3] + im->rgb[i * 3 + 1] + im->rgb[i * 3 + 2]) / 3.0; sumL += L * alpha[i]; sumA += alpha[i]; }
    const double Lm = sumA > 0 ? sumL / sumA : 128;
    for (size_t i = 0; i < n; i++) {
        const float a = alpha[i]; if (a <= 0) continue;
        uint8_t *p = im->rgb + i * 3; const double L = (p[0] + p[1] + p[2]) / 3.0, sh = (L - Lm) * 0.6;
        for (int c = 0; c < 3; c++) { double v = tc[c] + sh; v = v < 0 ? 0 : v > 255 ? 255 : v; p[c] = clip8(p[c] * (1 - a) + v * a); }
    }
}

ds4_img img_overlay(const ds4_img *im, const float *alpha, uint32_t rgb) {
    const size_t n = (size_t)im->w * im->h; ds4_img o = { im->w, im->h, malloc(n * 3) }; const double tc[3] = { (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255 };
    for (size_t i = 0; i < n; i++) { const double a = 0.55 * alpha[i]; for (int c = 0; c < 3; c++) o.rgb[i * 3 + c] = clip8(im->rgb[i * 3 + c] * (1 - a) + tc[c] * a); }
    return o;
}

double img_alpha_share(const float *alpha, size_t n) { double s = 0; for (size_t i = 0; i < n; i++) s += alpha[i] > 0.5f; return n ? s / n : 0; }

img_rect img_alpha_bbox(const float *alpha, int w, int h) {
    img_rect r = { w, h, -1, -1 }; int x1 = -1, y1 = -1;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (alpha[(size_t)y * w + x] > 0.5f) { if (x < r.x) r.x = x; if (y < r.y) r.y = y; if (x > x1) x1 = x; if (y > y1) y1 = y; }
    if (x1 < 0) { img_rect e = { 0, 0, 0, 0 }; return e; }
    r.w = x1 - r.x + 1; r.h = y1 - r.y + 1; return r;
}
