/* img_mask.c — 指点换色的颜色流水线(方案稿 docs/assistant_image.md §3.5 定稿, numpy 探针 7 轮验证后转录成 C):
 *   半分辨率 → 7×7 均值模糊 → 每个正/负点周围 24×24 像素各一个高斯分量(协方差 +9I) + 边框一圈每 32 px 一块当背景分量(NLL +4 弱先验, 永不丢)
 *   → 逐像素 min-NLL 竞争 → 补洞(从边框泛洪背景) → 只留与正点连通的块 → 7×7 多数滤波两遍 → 再补洞/连通 → 最近邻放回全分辨率。
 * 为什么是点不是框: 模型(DS 视觉)报点 7/7 全中、报框每次缩水 300~450 px(10-15 探针)。
 * 为什么不自迭代重估(GrabCut 式): 探针里收了左边吃右下; 漏了让模型看叠红图再加点。
 * 这条路是 ds4-seg(MobileSAM)不在时的退路: 颜色分得开的东西它够用, 花纹/同色贴同色不行。 */
#include "ds4_image.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define S 2            /* 半分辨率 */
#define PATCH 12       /* 种子块半径(半分辨率像素) */
#define RING_STEP 32
#define RING_INSET 10
#define RING_PEN 4.0f
#define MAX_COMP 512

typedef struct { float mu[3], ci[9], ld; } gcomp;   /* 高斯分量: 均值、协方差逆、log det */

static bool inv3(const double m[9], double o[9]) {
    const double d = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (fabs(d) < 1e-9) return false;
    o[0] = (m[4] * m[8] - m[5] * m[7]) / d; o[1] = (m[2] * m[7] - m[1] * m[8]) / d; o[2] = (m[1] * m[5] - m[2] * m[4]) / d;
    o[3] = (m[5] * m[6] - m[3] * m[8]) / d; o[4] = (m[0] * m[8] - m[2] * m[6]) / d; o[5] = (m[2] * m[3] - m[0] * m[5]) / d;
    o[6] = (m[3] * m[7] - m[4] * m[6]) / d; o[7] = (m[1] * m[6] - m[0] * m[7]) / d; o[8] = (m[0] * m[4] - m[1] * m[3]) / d;
    return true;
}
/* F = 模糊后的半分辨率 RGB [h][w][3]; 以 (x,y) 为中心 ±PATCH 的块 → 一个分量 */
static gcomp comp_at(const float *F, int w, int h, int x, int y) {
    gcomp g; const int x0 = x - PATCH < 0 ? 0 : x - PATCH, y0 = y - PATCH < 0 ? 0 : y - PATCH, x1 = x + PATCH > w ? w : x + PATCH, y1 = y + PATCH > h ? h : y + PATCH;
    double mu[3] = {0, 0, 0}, cov[9] = {0}; int n = 0;
    for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) { const float *p = F + ((size_t)yy * w + xx) * 3; for (int c = 0; c < 3; c++) mu[c] += p[c]; n++; }
    for (int c = 0; c < 3; c++) mu[c] /= n > 0 ? n : 1;
    for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) { const float *p = F + ((size_t)yy * w + xx) * 3; for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) cov[a * 3 + b] += (p[a] - mu[a]) * (p[b] - mu[b]); }
    for (int i = 0; i < 9; i++) cov[i] = cov[i] / (n > 1 ? n - 1 : 1) + (i % 4 == 0 ? 9.0 : 0.0);   /* +9I: 块太平也有宽度 */
    double ci[9]; if (!inv3(cov, ci)) { memset(ci, 0, sizeof ci); ci[0] = ci[4] = ci[8] = 1.0 / 9.0; }
    const double det = cov[0] * (cov[4] * cov[8] - cov[5] * cov[7]) - cov[1] * (cov[3] * cov[8] - cov[5] * cov[6]) + cov[2] * (cov[3] * cov[7] - cov[4] * cov[6]);
    for (int c = 0; c < 3; c++) g.mu[c] = (float)mu[c]; for (int i = 0; i < 9; i++) g.ci[i] = (float)ci[i]; g.ld = (float)log(det > 1e-12 ? det : 1e-12);
    return g;
}
static float nll(const gcomp *g, const float *p) {
    const float d0 = p[0] - g->mu[0], d1 = p[1] - g->mu[1], d2 = p[2] - g->mu[2];
    const float q = d0 * (g->ci[0] * d0 + g->ci[1] * d1 + g->ci[2] * d2) + d1 * (g->ci[3] * d0 + g->ci[4] * d1 + g->ci[5] * d2) + d2 * (g->ci[6] * d0 + g->ci[7] * d1 + g->ci[8] * d2);
    return q + g->ld;
}

/* 泛洪: 从 seeds 出发在 mask 为真的像素上 4 连通扩散, 返回到达集 */
static uint8_t *flood(const uint8_t *mask, int w, int h, const int *seeds, int nseeds) {
    uint8_t *seen = calloc((size_t)w * h, 1); int *q = malloc(sizeof(int) * (size_t)w * h); size_t qh = 0, qt = 0;
    for (int i = 0; i < nseeds; i++) { const int s = seeds[i]; if (s >= 0 && s < w * h && mask[s] && !seen[s]) { seen[s] = 1; q[qt++] = s; } }
    while (qh < qt) {
        const int s = q[qh++], x = s % w, y = s / w; const int nb[4] = { y > 0 ? s - w : -1, y < h - 1 ? s + w : -1, x > 0 ? s - 1 : -1, x < w - 1 ? s + 1 : -1 };
        for (int k = 0; k < 4; k++) if (nb[k] >= 0 && mask[nb[k]] && !seen[nb[k]]) { seen[nb[k]] = 1; q[qt++] = nb[k]; }
    }
    free(q); return seen;
}
/* 补洞 + 只留含正点的连通块 */
static void clean(uint8_t *m, int w, int h, const int *pos_idx, int npos) {
    uint8_t *inv = malloc((size_t)w * h); for (size_t i = 0; i < (size_t)w * h; i++) inv[i] = !m[i];
    int *border = malloc(sizeof(int) * (size_t)(2 * w + 2 * h)); int nb = 0;
    for (int x = 0; x < w; x++) { border[nb++] = x; border[nb++] = (h - 1) * w + x; } for (int y = 0; y < h; y++) { border[nb++] = y * w; border[nb++] = y * w + w - 1; }
    uint8_t *bg = flood(inv, w, h, border, nb);
    for (size_t i = 0; i < (size_t)w * h; i++) m[i] = !bg[i];
    uint8_t *keep = flood(m, w, h, pos_idx, npos);
    memcpy(m, keep, (size_t)w * h);
    free(inv); free(border); free(bg); free(keep);
}
static void majority(uint8_t *m, int w, int h) {   /* 7×7 多数滤波: 磨掉罗纹造成的逐行翻转 */
    float *f = malloc(sizeof(float) * (size_t)w * h), *o = malloc(sizeof(float) * (size_t)w * h);
    for (size_t i = 0; i < (size_t)w * h; i++) f[i] = m[i];
    img_boxblur_f(f, w, h, 3, o);
    for (size_t i = 0; i < (size_t)w * h; i++) m[i] = o[i] > 0.5f;
    free(f); free(o);
}

uint8_t *img_mask_color(const ds4_img *im, const int *pos, int npos, const int *neg, int nneg) {
    const int W = im->w, H = im->h, w = W / S, h = H / S;
    /* 半分辨率(2×2 均值) → 7×7 均值模糊 */
    float *a = malloc(sizeof(float) * (size_t)w * h * 3), *F = malloc(sizeof(float) * (size_t)w * h * 3), *ch = malloc(sizeof(float) * (size_t)w * h), *cho = malloc(sizeof(float) * (size_t)w * h);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int c = 0; c < 3; c++) {
        float s = 0; for (int dy = 0; dy < S; dy++) for (int dx = 0; dx < S; dx++) s += im->rgb[((size_t)(y * S + dy) * W + x * S + dx) * 3 + c];
        a[((size_t)y * w + x) * 3 + c] = s / (S * S);
    }
    for (int c = 0; c < 3; c++) { for (size_t i = 0; i < (size_t)w * h; i++) ch[i] = a[i * 3 + c]; img_boxblur_f(ch, w, h, 3, cho); for (size_t i = 0; i < (size_t)w * h; i++) F[i * 3 + c] = cho[i]; }
    free(a); free(ch); free(cho);
    /* 分量: 正点 / 负点 / 边框环 */
    gcomp *pc = malloc(sizeof(gcomp) * MAX_COMP), *nc = malloc(sizeof(gcomp) * MAX_COMP); int np = 0, nn = 0; int *pidx = malloc(sizeof(int) * (size_t)(npos > 0 ? npos : 1));
    for (int i = 0; i < npos && np < MAX_COMP; i++) { int x = pos[2 * i] / S, y = pos[2 * i + 1] / S; if (x < 0) x = 0; if (y < 0) y = 0; if (x >= w) x = w - 1; if (y >= h) y = h - 1; pc[np++] = comp_at(F, w, h, x, y); pidx[i] = y * w + x; }
    for (int i = 0; i < nneg && nn < MAX_COMP; i++) { int x = neg[2 * i] / S, y = neg[2 * i + 1] / S; if (x < 0) x = 0; if (y < 0) y = 0; if (x >= w) x = w - 1; if (y >= h) y = h - 1; nc[nn++] = comp_at(F, w, h, x, y); }
    const int nneg_explicit = nn, inset = RING_INSET < w / 2 ? RING_INSET : w / 2 - 1;
    for (int x = inset; x < w - inset && nn + 2 <= MAX_COMP; x += RING_STEP) { nc[nn++] = comp_at(F, w, h, x, inset); nc[nn++] = comp_at(F, w, h, x, h - 1 - inset); }
    for (int y = inset; y < h - inset && nn + 2 <= MAX_COMP; y += RING_STEP) { nc[nn++] = comp_at(F, w, h, inset, y); nc[nn++] = comp_at(F, w, h, w - 1 - inset, y); }
    /* 竞争 */
    uint8_t *m = malloc((size_t)w * h);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        const float *p = F + i * 3; float bp = 1e30f, bn = 1e30f;
        for (int k = 0; k < np; k++) { const float v = nll(&pc[k], p); if (v < bp) bp = v; }
        for (int k = 0; k < nn; k++) { const float v = nll(&nc[k], p) + (k >= nneg_explicit ? RING_PEN : 0.0f); if (v < bn) bn = v; }
        m[i] = bp < bn;
    }
    clean(m, w, h, pidx, npos);
    majority(m, w, h); majority(m, w, h);
    clean(m, w, h, pidx, npos);
    /* 最近邻放回全分辨率 */
    uint8_t *full = malloc((size_t)W * H);
    for (int y = 0; y < H; y++) { const int sy = y / S < h ? y / S : h - 1; for (int x = 0; x < W; x++) { const int sx = x / S < w ? x / S : w - 1; full[(size_t)y * W + x] = m[(size_t)sy * w + sx]; } }
    free(m); free(F); free(pc); free(nc); free(pidx);
    return full;
}
