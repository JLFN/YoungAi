/* core_v41_image.c — 图片字节 → ViT patch(2026-10-13)。官方 inference/image_processor.py 逐式转录。
 *
 * 三步, 每步都有"错了会怎样":
 *  ① 尺寸账(plan_image_grid / safe_resize / solve_resize_ratio): 原图先按 min_pixels 放大(小图)、再按 14 取整、
 *     超 max_image_tokens 就按长宽比求最大可装尺寸。算错一格, span 长度就与模型见过的分布不同 —— 不报错, 识别率掉。
 *  ② 缩放补边(ImageOps.pad): PIL 的双三次 = 带抗锯齿的可分离卷积 + 22 位定点累加 + 8 位截断; 这里逐式转录(含 Python round 的
 *     "四舍六入五成双"), 同一张图与 PIL 逐像素同(JPEG 解码用 stb_image, 与 libjpeg 的 IDCT 可差 ±1, 那是解码器的差, 不是这里的)。
 *     用 OpenCV/朴素双线性的后果: 像素整体偏 1~3, ViT 输入分布漂移, 看图细节(小字、表格线)最先糊。
 *  ③ 归一化 (x/255 − 0.5)/0.5 舍 bf16, 按 [patch][c][ph][pw] 切(官方 reshape/permute 的顺序, Linear 的 588 维输入就按这个顺序)。 */
#include "core_internal.h"
#include "src/common/ds4_float.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "stb_image.h"
#pragma GCC diagnostic pop

static uint64_t ceil_div_u64(uint64_t a, uint64_t b) { return (a + b - 1) / b; }
static uint32_t span_tokens(uint32_t lh, uint32_t lw) { return lh * (lw + 1u) + 2u; }   /* 官方 num_image_tokens */

bool v41_image_plan(uint32_t w0, uint32_t h0, uint32_t *n_vit_h, uint32_t *n_vit_w, uint32_t *n_llm_h, uint32_t *n_llm_w, uint32_t *best_w, uint32_t *best_h) {
    const ds4_v41_cfg *v = &g_ds4_v41;
    const uint64_t p = v->vis_patch, r = v->vis_ds, maxt = v->vis_max_tok;
    if (!w0 || !h0 || !p || !r || maxt < 4) return false;
    long long w = w0, h = h0;   /* Python int */
    if ((double)w * (double)h < (double)v->vis_min_px) {   /* 小图放大到 min_pixels(官方 int() 截断) */
        const double ratio = sqrt((double)v->vis_min_px / ((double)w * (double)h));
        w = (long long)((double)w * ratio); h = (long long)((double)h * ratio);
    }
    uint64_t bw = ceil_div_u64((uint64_t)w, p) * p, bh = ceil_div_u64((uint64_t)h, p) * p;
    uint64_t lh = ceil_div_u64(bh / p, r), lw = ceil_div_u64(bw / p, r);
    if (span_tokens((uint32_t)lh, (uint32_t)lw) > maxt) {   /* 官方 solve_resize_ratio: 长宽比不变的最大可装尺寸 */
        const double rr = (double)h / (double)w;
        const double max_w = sqrt((double)(maxt - 2) / rr + 0.25) - 0.5, max_h = max_w * rr;
        const uint64_t cell = p * r;
        if (max_w < 1.0) { bh = (maxt - 2) / 2 * cell; bw = cell; }
        else if (max_h < 1.0) { bh = cell; bw = (maxt - 3) * cell; }
        else {
            const double bw_ = floor(max_w) * (double)cell / (double)w, bh_ = floor(max_h) * (double)cell / (double)h;
            const double beta = bw_ < bh_ ? bw_ : bh_;
            bh = (uint64_t)floor((double)h * beta / (double)p) * p;
            bw = (uint64_t)floor((double)w * beta / (double)p) * p;
        }
        lh = ceil_div_u64(bh / p, r); lw = ceil_div_u64(bw / p, r);
        if (span_tokens((uint32_t)lh, (uint32_t)lw) > maxt) return false;
    }
    if (!bw || !bh) return false;
    *n_vit_h = (uint32_t)(bh / p); *n_vit_w = (uint32_t)(bw / p); *n_llm_h = (uint32_t)lh; *n_llm_w = (uint32_t)lw;
    *best_w = (uint32_t)bw; *best_h = (uint32_t)bh;
    return true;
}

/* ---- PIL 双三次重采样的逐式转录(libImaging/Resample.c, 8 位 RGB 路) ---- */
#define PIL_PRECISION_BITS 22
static double pil_bicubic(double x) {
    const double a = -0.5;
    if (x < 0.0) x = -x;
    if (x < 1.0) return ((a + 2.0) * x - (a + 3.0)) * x * x + 1.0;
    if (x < 2.0) return (((x - 5.0) * x + 8.0) * x - 4.0) * a;
    return 0.0;
}
/* 每个输出坐标的 [起点, 个数] 与 22 位定点系数(含抗锯齿: 缩小时核按缩放比拉宽) */
static int pil_coeffs(uint32_t in_size, uint32_t out_size, int **bounds_out, int32_t **kk_out) {
    const double scale = (double)in_size / (double)out_size, fscale = scale < 1.0 ? 1.0 : scale;
    const double support = 2.0 * fscale;
    const int ksize = (int)ceil(support) * 2 + 1;
    double *kk = xmalloc(sizeof(double) * (size_t)out_size * (size_t)ksize);
    int32_t *kq = xmalloc(sizeof(int32_t) * (size_t)out_size * (size_t)ksize);
    int *bounds = xmalloc(sizeof(int) * (size_t)out_size * 2);
    for (uint32_t xx = 0; xx < out_size; xx++) {
        const double center = ((double)xx + 0.5) * scale, ss = 1.0 / fscale;
        int xmin = (int)(center - support + 0.5); if (xmin < 0) xmin = 0;
        int xmax = (int)(center + support + 0.5); if (xmax > (int)in_size) xmax = (int)in_size;
        xmax -= xmin;
        double *k = kk + (size_t)xx * ksize, ww = 0.0;
        int x;
        for (x = 0; x < xmax; x++) { k[x] = pil_bicubic((x + xmin - center + 0.5) * ss); ww += k[x]; }
        for (x = 0; x < xmax; x++) if (ww != 0.0) k[x] /= ww;
        for (; x < ksize; x++) k[x] = 0.0;
        bounds[xx * 2] = xmin; bounds[xx * 2 + 1] = xmax;
    }
    for (size_t i = 0; i < (size_t)out_size * ksize; i++) {   /* normalize_coeffs_8bpc */
        const double v = kk[i] * (double)(1 << PIL_PRECISION_BITS);
        kq[i] = v < 0 ? (int32_t)(-0.5 + v) : (int32_t)(0.5 + v);
    }
    free(kk);
    *bounds_out = bounds; *kk_out = kq;
    return ksize;
}
static inline uint8_t pil_clip8(int32_t ss) {
    const int32_t v = ss >> PIL_PRECISION_BITS;   /* 算术右移(gcc/clang), 与 PIL 同 */
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}
/* 先横后纵(ImagingResampleInner 的顺序); 任一维尺寸不变就跳过那一遍(PIL 同) */
static uint8_t *pil_resize_rgb(const uint8_t *in, uint32_t w, uint32_t h, uint32_t tw, uint32_t th) {
    uint8_t *cur = xmalloc((size_t)w * h * 3); memcpy(cur, in, (size_t)w * h * 3);
    uint32_t cw = w;
    if (tw != w) {
        int *b; int32_t *kk; const int ks = pil_coeffs(w, tw, &b, &kk);
        uint8_t *out = xmalloc((size_t)tw * h * 3);
        for (uint32_t y = 0; y < h; y++) for (uint32_t xx = 0; xx < tw; xx++) {
            const int xmin = b[xx * 2], xmax = b[xx * 2 + 1]; const int32_t *k = kk + (size_t)xx * ks;
            int32_t s0 = 1 << (PIL_PRECISION_BITS - 1), s1 = s0, s2 = s0;
            for (int x = 0; x < xmax; x++) { const uint8_t *px = cur + ((size_t)y * w + (size_t)(x + xmin)) * 3; s0 += px[0] * k[x]; s1 += px[1] * k[x]; s2 += px[2] * k[x]; }
            uint8_t *o = out + ((size_t)y * tw + xx) * 3; o[0] = pil_clip8(s0); o[1] = pil_clip8(s1); o[2] = pil_clip8(s2);
        }
        free(b); free(kk); free(cur); cur = out; cw = tw;
    }
    if (th != h) {
        int *b; int32_t *kk; const int ks = pil_coeffs(h, th, &b, &kk);
        uint8_t *out = xmalloc((size_t)cw * th * 3);
        for (uint32_t yy = 0; yy < th; yy++) {
            const int ymin = b[yy * 2], ymax = b[yy * 2 + 1]; const int32_t *k = kk + (size_t)yy * ks;
            for (uint32_t x = 0; x < cw; x++) {
                int32_t s0 = 1 << (PIL_PRECISION_BITS - 1), s1 = s0, s2 = s0;
                for (int y = 0; y < ymax; y++) { const uint8_t *px = cur + ((size_t)(y + ymin) * cw + x) * 3; s0 += px[0] * k[y]; s1 += px[1] * k[y]; s2 += px[2] * k[y]; }
                uint8_t *o = out + ((size_t)yy * cw + x) * 3; o[0] = pil_clip8(s0); o[1] = pil_clip8(s1); o[2] = pil_clip8(s2);
            }
        }
        free(b); free(kk); free(cur); cur = out;
    }
    return cur;
}
static long py_round(double v) { return (long)nearbyint(v); }   /* Python round(): 四舍六入五成双 = IEEE 默认舍入 */

uint8_t *v41_image_resize_pad(const uint8_t *rgb, uint32_t w, uint32_t h, uint32_t best_w, uint32_t best_h) {
    /* ImageOps.contain: 等比缩到装进 (best_w, best_h); PIL 的比较与 round 顺序原样 */
    uint32_t tw = best_w, th = best_h;
    const double im_ratio = (double)w / (double)h, dest_ratio = (double)best_w / (double)best_h;
    if (im_ratio != dest_ratio) {
        if (im_ratio > dest_ratio) { const long nh = py_round((double)h / (double)w * (double)best_w); if (nh != (long)best_h) th = (uint32_t)(nh > 0 ? nh : 1); }
        else { const long nw = py_round((double)w / (double)h * (double)best_h); if (nw != (long)best_w) tw = (uint32_t)(nw > 0 ? nw : 1); }
    }
    uint8_t *rs = pil_resize_rgb(rgb, w, h, tw, th);
    if (tw == best_w && th == best_h) return rs;
    /* ImageOps.pad: 灰 (127,127,127) 底, 居中(round(差 × 0.5)) */
    uint8_t *out = xmalloc((size_t)best_w * best_h * 3);
    memset(out, 127, (size_t)best_w * best_h * 3);
    const uint32_t x0 = tw != best_w ? (uint32_t)py_round((double)(best_w - tw) * 0.5) : 0u;
    const uint32_t y0 = tw != best_w ? 0u : (uint32_t)py_round((double)(best_h - th) * 0.5);
    for (uint32_t y = 0; y < th; y++) memcpy(out + ((size_t)(y + y0) * best_w + x0) * 3, rs + (size_t)y * tw * 3, (size_t)tw * 3);
    free(rs);
    return out;
}

float *v41_image_patches(const uint8_t *bytes, size_t len, uint32_t *n_vit_h, uint32_t *n_vit_w, uint32_t *n_llm_h, uint32_t *n_llm_w, char *err, size_t errn) {
    const ds4_v41_cfg *v = &g_ds4_v41;
    int w = 0, h = 0, comp = 0;
    uint8_t *rgb = stbi_load_from_memory(bytes, (int)len, &w, &h, &comp, 3);   /* 3 = 丢 alpha 取 RGB(PIL convert("RGB") 同样不做合成) */
    if (!rgb) { snprintf(err, errn, "图片解不开: %s", stbi_failure_reason()); return NULL; }
    uint32_t bw = 0, bh = 0;
    if (!v41_image_plan((uint32_t)w, (uint32_t)h, n_vit_h, n_vit_w, n_llm_h, n_llm_w, &bw, &bh)) {
        snprintf(err, errn, "图片 %dx%d 装不进 %u 个 token 的尺寸账", w, h, v->vis_max_tok); stbi_image_free(rgb); return NULL;
    }
    uint8_t *px = v41_image_resize_pad(rgb, (uint32_t)w, (uint32_t)h, bw, bh);
    stbi_image_free(rgb);
    const uint32_t p = v->vis_patch, nh = *n_vit_h, nw = *n_vit_w, pp = p * p;
    float *pt = xmalloc((size_t)nh * nw * 3 * pp * sizeof(float));
    for (uint32_t i = 0; i < nh; i++) for (uint32_t j = 0; j < nw; j++) {
        float *dst = pt + ((size_t)i * nw + j) * 3 * pp;
        for (uint32_t c = 0; c < 3; c++) for (uint32_t ph = 0; ph < p; ph++) for (uint32_t pw = 0; pw < p; pw++) {
            const float x = (float)px[((size_t)(i * p + ph) * bw + (j * p + pw)) * 3 + c] / 255.0f;
            dst[c * pp + ph * p + pw] = ds4_bf16_round_f32((x - 0.5f) / 0.5f);
        }
    }
    free(px);
    return pt;
}
