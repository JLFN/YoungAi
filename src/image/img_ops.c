/* img_ops.c — 几何(裁/缩/转/翻)、调色、模糊/马赛克、画框线箭头、贴图。全部就地改 ds4_img(缩放/旋转换掉像素缓冲)。
 * 缩放是 PIL BILINEAR 的转录(缩小时三角核支撑按比例放大): 和 ds4-seg 预处理共用同一份, 不许再抄一份。 */
#include "ds4_image.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint8_t clip8(double v) { v = floor(v + 0.5); return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }
static img_rect whole(const ds4_img *im, img_rect r) {   /* 空矩形 = 整图; 其余夹进图内 */
    if (r.w <= 0 || r.h <= 0) { img_rect a = { 0, 0, im->w, im->h }; return a; }
    if (r.x < 0) { r.w += r.x; r.x = 0; } if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > im->w) r.w = im->w - r.x; if (r.y + r.h > im->h) r.h = im->h - r.y;
    if (r.w < 0) r.w = 0; if (r.h < 0) r.h = 0;
    return r;
}

/* 一维三角核表(PIL ImagingResampleHorizontal): 每个输出位置 → 输入起点 + 归一化系数 */
typedef struct { int *xmin, *xsize; double *k; int ksize; } kern;
static kern make_kern(int in_size, int out_size) {
    kern K; const double scale = (double)in_size / out_size, fscale = scale < 1.0 ? 1.0 : scale, support = 1.0 * fscale;
    K.ksize = (int)ceil(support) * 2 + 1;
    K.xmin = malloc(sizeof(int) * (size_t)out_size); K.xsize = malloc(sizeof(int) * (size_t)out_size); K.k = calloc((size_t)out_size * (size_t)K.ksize, sizeof(double));
    for (int o = 0; o < out_size; o++) {
        const double center = (o + 0.5) * scale, ss = 1.0 / fscale;
        int xmin = (int)(center - support + 0.5); if (xmin < 0) xmin = 0;
        int xmax = (int)(center + support + 0.5); if (xmax > in_size) xmax = in_size;
        double *k = K.k + (size_t)o * K.ksize, ww = 0; int x;
        for (x = 0; x < xmax - xmin; x++) { const double t = fabs((x + xmin - center + 0.5) * ss); k[x] = t < 1.0 ? 1.0 - t : 0.0; ww += k[x]; }
        if (ww) for (int i = 0; i < x; i++) k[i] /= ww;
        K.xmin[o] = xmin; K.xsize[o] = x;
    }
    return K;
}
static void free_kern(kern *K) { free(K->xmin); free(K->xsize); free(K->k); }

uint8_t *img_resize_ch(const uint8_t *in, int w, int h, int ch, int tw, int th) {
    kern kx = make_kern(w, tw), ky = make_kern(h, th);
    uint8_t *tmp = malloc((size_t)tw * h * ch), *out = malloc((size_t)tw * th * ch); double s[8];
    for (int y = 0; y < h; y++) for (int o = 0; o < tw; o++) {   /* 先横向 */
        const double *k = kx.k + (size_t)o * kx.ksize; for (int c = 0; c < ch; c++) s[c] = 0;
        for (int i = 0; i < kx.xsize[o]; i++) { const uint8_t *p = in + ((size_t)y * w + kx.xmin[o] + i) * ch; for (int c = 0; c < ch; c++) s[c] += p[c] * k[i]; }
        uint8_t *q = tmp + ((size_t)y * tw + o) * ch; for (int c = 0; c < ch; c++) q[c] = clip8(s[c]);
    }
    for (int o = 0; o < th; o++) for (int x = 0; x < tw; x++) {   /* 再纵向 */
        const double *k = ky.k + (size_t)o * ky.ksize; for (int c = 0; c < ch; c++) s[c] = 0;
        for (int i = 0; i < ky.xsize[o]; i++) { const uint8_t *p = tmp + ((size_t)(ky.xmin[o] + i) * tw + x) * ch; for (int c = 0; c < ch; c++) s[c] += p[c] * k[i]; }
        uint8_t *q = out + ((size_t)o * tw + x) * ch; for (int c = 0; c < ch; c++) q[c] = clip8(s[c]);
    }
    free(tmp); free_kern(&kx); free_kern(&ky);
    return out;
}

bool img_crop(ds4_img *im, img_rect r) {
    r = whole(im, r); if (r.w < 1 || r.h < 1) return false;
    uint8_t *out = malloc((size_t)r.w * r.h * 3);
    for (int y = 0; y < r.h; y++) memcpy(out + (size_t)y * r.w * 3, im->rgb + ((size_t)(r.y + y) * im->w + r.x) * 3, (size_t)r.w * 3);
    free(im->rgb); im->rgb = out; im->w = r.w; im->h = r.h; return true;
}
void img_resize(ds4_img *im, int tw, int th) {
    if (tw < 1 || th < 1 || (tw == im->w && th == im->h)) return;
    uint8_t *out = img_resize_ch(im->rgb, im->w, im->h, 3, tw, th); free(im->rgb); im->rgb = out; im->w = tw; im->h = th;
}
void img_rotate(ds4_img *im, int deg) {
    deg = ((deg % 360) + 360) % 360; if (!deg) return;
    const int W = im->w, H = im->h, nw = deg == 180 ? W : H, nh = deg == 180 ? H : W;
    uint8_t *out = malloc((size_t)W * H * 3);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int nx, ny;
        if (deg == 90) { nx = H - 1 - y; ny = x; } else if (deg == 180) { nx = W - 1 - x; ny = H - 1 - y; } else { nx = y; ny = W - 1 - x; }
        memcpy(out + ((size_t)ny * nw + nx) * 3, im->rgb + ((size_t)y * W + x) * 3, 3);
    }
    free(im->rgb); im->rgb = out; im->w = nw; im->h = nh;
}
void img_flip(ds4_img *im, bool horizontal) {
    const int W = im->w, H = im->h; uint8_t px[3];
    if (horizontal) { for (int y = 0; y < H; y++) for (int x = 0; x < W / 2; x++) { uint8_t *a = im->rgb + ((size_t)y * W + x) * 3, *b = im->rgb + ((size_t)y * W + W - 1 - x) * 3; memcpy(px, a, 3); memcpy(a, b, 3); memcpy(b, px, 3); } }
    else { uint8_t *row = malloc((size_t)W * 3); for (int y = 0; y < H / 2; y++) { uint8_t *a = im->rgb + (size_t)y * W * 3, *b = im->rgb + (size_t)(H - 1 - y) * W * 3; memcpy(row, a, (size_t)W * 3); memcpy(a, b, (size_t)W * 3); memcpy(b, row, (size_t)W * 3); } free(row); }
}

/* 亮度: ±128 平移; 对比: 绕 128 拉伸 (1 + c/100); 饱和: 绕灰度拉伸 (1 + s/100) */
void img_adjust(ds4_img *im, img_rect r, float brightness, float contrast, float saturation) {
    r = whole(im, r); const double kb = brightness / 100.0 * 128.0, kc = 1.0 + contrast / 100.0, ks = 1.0 + saturation / 100.0;
    for (int y = r.y; y < r.y + r.h; y++) for (int x = r.x; x < r.x + r.w; x++) {
        uint8_t *p = im->rgb + ((size_t)y * im->w + x) * 3; double v[3], L = 0;
        for (int c = 0; c < 3; c++) { v[c] = (p[c] - 128.0) * kc + 128.0 + kb; L += v[c] / 3; }
        for (int c = 0; c < 3; c++) p[c] = clip8(L + (v[c] - L) * ks);
    }
}

/* 区域内积分图均值(框外像素按最近延拓: 把区域当一张小图) */
static void region_box(ds4_img *im, img_rect r, int radius, uint8_t (*fn)(double)) {
    (void)fn;
    const int W = r.w, H = r.h; if (W < 1 || H < 1 || radius < 1) return;
    double *sat = calloc((size_t)(W + 1) * (H + 1) * 3, sizeof(double));
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) for (int c = 0; c < 3; c++) {
        const double v = im->rgb[((size_t)(r.y + y) * im->w + r.x + x) * 3 + c];
        sat[((size_t)(y + 1) * (W + 1) + x + 1) * 3 + c] = v + sat[((size_t)y * (W + 1) + x + 1) * 3 + c] + sat[((size_t)(y + 1) * (W + 1) + x) * 3 + c] - sat[((size_t)y * (W + 1) + x) * 3 + c];
    }
    uint8_t *out = malloc((size_t)W * H * 3);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        const int y0 = y - radius < 0 ? 0 : y - radius, x0 = x - radius < 0 ? 0 : x - radius, y1 = y + radius + 1 > H ? H : y + radius + 1, x1 = x + radius + 1 > W ? W : x + radius + 1;
        const double area = (double)(y1 - y0) * (x1 - x0);
        for (int c = 0; c < 3; c++) out[((size_t)y * W + x) * 3 + c] = clip8((sat[((size_t)y1 * (W + 1) + x1) * 3 + c] - sat[((size_t)y0 * (W + 1) + x1) * 3 + c] - sat[((size_t)y1 * (W + 1) + x0) * 3 + c] + sat[((size_t)y0 * (W + 1) + x0) * 3 + c]) / area);
    }
    for (int y = 0; y < H; y++) memcpy(im->rgb + ((size_t)(r.y + y) * im->w + r.x) * 3, out + (size_t)y * W * 3, (size_t)W * 3);
    free(out); free(sat);
}
void img_blur(ds4_img *im, img_rect r, int radius) { r = whole(im, r); region_box(im, r, radius, NULL); region_box(im, r, radius, NULL); }   /* 两遍均值 ≈ 三角核, 比一遍少方块感 */
void img_pixelate(ds4_img *im, img_rect r, int block) {
    r = whole(im, r); if (block < 2) block = 2;
    for (int by = r.y; by < r.y + r.h; by += block) for (int bx = r.x; bx < r.x + r.w; bx += block) {
        const int y1 = by + block < r.y + r.h ? by + block : r.y + r.h, x1 = bx + block < r.x + r.w ? bx + block : r.x + r.w; double s[3] = {0, 0, 0}; int n = 0;
        for (int y = by; y < y1; y++) for (int x = bx; x < x1; x++) { const uint8_t *p = im->rgb + ((size_t)y * im->w + x) * 3; s[0] += p[0]; s[1] += p[1]; s[2] += p[2]; n++; }
        for (int y = by; y < y1; y++) for (int x = bx; x < x1; x++) { uint8_t *p = im->rgb + ((size_t)y * im->w + x) * 3; p[0] = clip8(s[0] / n); p[1] = clip8(s[1] / n); p[2] = clip8(s[2] / n); }
    }
}

static void put(ds4_img *im, int x, int y, uint32_t rgb) { if (x < 0 || y < 0 || x >= im->w || y >= im->h) return; uint8_t *p = im->rgb + ((size_t)y * im->w + x) * 3; p[0] = (uint8_t)(rgb >> 16); p[1] = (uint8_t)(rgb >> 8); p[2] = (uint8_t)rgb; }
void img_draw_rect(ds4_img *im, img_rect r, uint32_t rgb, int width, bool fill) {
    if (width < 1) width = 1;
    for (int y = r.y - (fill ? 0 : 0); y < r.y + r.h; y++) for (int x = r.x; x < r.x + r.w; x++) {
        const bool edge = x < r.x + width || x >= r.x + r.w - width || y < r.y + width || y >= r.y + r.h - width;
        if (fill || edge) put(im, x, y, rgb);
    }
}
void img_draw_ellipse(ds4_img *im, img_rect r, uint32_t rgb, int width, bool fill) {
    if (width < 1) width = 1;
    const double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0, ax = r.w / 2.0, ay = r.h / 2.0, ix = ax - width > 0 ? ax - width : 0, iy = ay - width > 0 ? ay - width : 0;
    for (int y = r.y; y < r.y + r.h; y++) for (int x = r.x; x < r.x + r.w; x++) {
        const double dx = x + 0.5 - cx, dy = y + 0.5 - cy, outer = dx * dx / (ax * ax) + dy * dy / (ay * ay);
        if (outer > 1.0) continue;
        const double inner = ix > 0 && iy > 0 ? dx * dx / (ix * ix) + dy * dy / (iy * iy) : 2.0;
        if (fill || inner >= 1.0) put(im, x, y, rgb);
    }
}
static void thick_line(ds4_img *im, double x1, double y1, double x2, double y2, uint32_t rgb, double half) {
    const double dx = x2 - x1, dy = y2 - y1, len2 = dx * dx + dy * dy;
    const int X0 = (int)floor(fmin(x1, x2) - half - 1), X1 = (int)ceil(fmax(x1, x2) + half + 1), Y0 = (int)floor(fmin(y1, y2) - half - 1), Y1 = (int)ceil(fmax(y1, y2) + half + 1);
    for (int y = Y0; y <= Y1; y++) for (int x = X0; x <= X1; x++) {
        const double px = x + 0.5 - x1, py = y + 0.5 - y1; double t = len2 > 0 ? (px * dx + py * dy) / len2 : 0; t = t < 0 ? 0 : t > 1 ? 1 : t;
        const double ex = px - t * dx, ey = py - t * dy;
        if (ex * ex + ey * ey <= half * half) put(im, x, y, rgb);
    }
}
void img_draw_line(ds4_img *im, int x1, int y1, int x2, int y2, uint32_t rgb, int width, bool arrow) {
    if (width < 1) width = 1; const double half = width / 2.0;
    thick_line(im, x1, y1, x2, y2, rgb, half);
    if (!arrow) return;
    const double dx = x2 - x1, dy = y2 - y1, len = sqrt(dx * dx + dy * dy); if (len < 1) return;
    const double ux = dx / len, uy = dy / len, hl = 4.0 * width + 8, hw = 2.5 * width + 4;   /* 箭头长宽随线宽 */
    thick_line(im, x2, y2, x2 - ux * hl - uy * hw, y2 - uy * hl + ux * hw, rgb, half);
    thick_line(im, x2, y2, x2 - ux * hl + uy * hw, y2 - uy * hl - ux * hw, rgb, half);
}

void img_paste(ds4_img *im, const uint8_t *rgba, int sw, int sh, int x, int y, int tw, float opacity) {
    uint8_t *src = NULL; int w = sw, h = sh;
    if (tw > 0 && tw != sw) { h = (int)((double)sh * tw / sw + 0.5); if (h < 1) h = 1; w = tw; src = img_resize_ch(rgba, sw, sh, 4, w, h); rgba = src; }
    if (opacity <= 0 || opacity > 1) opacity = 1;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
        const int X = x + i, Y = y + j; if (X < 0 || Y < 0 || X >= im->w || Y >= im->h) continue;
        const uint8_t *s = rgba + ((size_t)j * w + i) * 4; const double a = s[3] / 255.0 * opacity; uint8_t *d = im->rgb + ((size_t)Y * im->w + X) * 3;
        for (int c = 0; c < 3; c++) d[c] = clip8(d[c] * (1 - a) + s[c] * a);
    }
    free(src);
}
