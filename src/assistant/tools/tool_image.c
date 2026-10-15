/* tool_image.c — edit_image / view_image 的执行点(方案稿 docs/assistant_image.md): 解参 → 带 points 的 op 先在主进程跑分割(tool_image_seg.c)
 * → fork 沙盒子进程按序做 ops → 成品 + 预览(+ 叠红掩膜 / 放大核对图)落工作区 → 结果句。
 * 为什么像素工作进沙盒: 解码的是用户传的图(stb_image 有 bug 也只炸子进程)。
 * 结果开头的 "image: 路径" 行由 asst_run.c 接成 image_url 块(模型直接看成品、掩膜和核对图, 偏了就改坐标再调)。 */
#include "tool_image.h"
#include <sys/stat.h>
#include <unistd.h>

#define PREVIEW_MAX 768    /* 带回给模型看的图长边: 全分辨率 PNG 一张 3~4 MB, 七轮下来请求体 70 MB 把模型连接撑死(10-15 实撞); 视觉塔本来也只看缩过的 */
#define ZOOM_MAX 1024      /* 放大核对图长边: 网格标签字高 21 px 才读得准(img_grid.c); 1024x768 进视觉塔不再缩 */
#define ZOOM_MIN_SIDE 0.2  /* 核对窗口至少是原图长边的 20%: 整图上指点偏约 10%, 窗口太小时真目标会落在窗外, 模型看不到该往哪挪 */
#define VIEW_MIN_SIDE 0.125 /* view_image 窗口至少是原图长边的 1/8: 放大超过 6 倍左右只剩糊, 模型看不清又再放大(10-15 真模型门: 45x40 的窗口放大 23 倍, 打码一条 13 次调用) */

static img_rect region_of(const char *op) {
    img_rect r = { 0, 0, 0, 0 }; char *raw = NULL;
    if (asst_jraw(op, "region", &raw) && raw) { long x = 0, y = 0, w = 0, h = 0; asst_jint(raw, "x", &x); asst_jint(raw, "y", &y); asst_jint(raw, "w", &w); asst_jint(raw, "h", &h); r.x = (int)x; r.y = (int)y; r.w = (int)w; r.h = (int)h; }
    free(raw); return r;
}
static long num(const char *op, const char *key, long dflt) { long v = dflt; asst_jint(op, key, &v); return v; }
static bool color_of(const char *op, uint32_t *rgb, ds4_buf *b) {
    char *c = NULL; asst_jstr(op, "color", &c); bool ok = false; *rgb = img_parse_color(c ? c : "", &ok);
    if (!ok) ds4_buf_printf(b, "error: color must be #rrggbb or a basic color name (got %s)\n", c ? c : "nothing");
    free(c); return ok;
}
static void rel(const image_job *j, const char *abs, char *out, size_t n) { if (asst_under(abs, j->ws) && strlen(abs) > strlen(j->ws)) snprintf(out, n, "%s", abs + strlen(j->ws) + 1); else snprintf(out, n, "%s", abs); }
/* 预览路径 .img/<成品名>.<tag>.png; 预览 = 长边缩到 PREVIEW_MAX 的 PNG */
static void preview_path(const image_job *j, const char *tag, char *out, size_t n) {
    const char *sl = strrchr(j->out, '/'); const char *base = sl ? sl + 1 : j->out; const char *dot = strrchr(base, '.');
    snprintf(out, n, "%s/.img/%.*s.%s.png", j->ws, (int)(dot ? dot - base : (long)strlen(base)), base, tag);
}
static bool write_preview(const ds4_img *im, const char *path, const char *trash) {
    int tw = im->w, th = im->h; const int L = im->w > im->h ? im->w : im->h;
    if (L > PREVIEW_MAX) { tw = (int)((double)im->w * PREVIEW_MAX / L + 0.5); th = (int)((double)im->h * PREVIEW_MAX / L + 0.5); if (tw < 1) tw = 1; if (th < 1) th = 1; }
    uint8_t *px = tw == im->w && th == im->h ? im->rgb : img_resize_ch(im->rgb, im->w, im->h, 3, tw, th);
    size_t n; uint8_t *png = img_png_encode(tw, th, 3, px, &n); if (px != im->rgb) free(px);
    char *werr = asst_fs_put_bytes(path, png, n, trash); free(png);
    if (werr) { free(werr); return false; }
    return true;
}

/* recolor: 掩膜来自第 i 个 op 的 ds4-seg PNG(灰度 ≥128 = 内)或颜色流水线; 叠红图落在成品旁 */
static bool recolor(const image_job *j, int i, const char *op, ds4_img *im, bool framed, ds4_buf *b, bool *overlay_done) {
    uint32_t rgb; if (!color_of(op, &rgb, b)) return false;
    char *praw = NULL, *araw = NULL; asst_jraw(op, "points", &praw); asst_jraw(op, "avoid", &araw);
    int pos[2 * MAX_PTS], neg[2 * MAX_PTS]; const int npos = img_parse_points(praw, pos, MAX_PTS), nneg = img_parse_points(araw, neg, MAX_PTS); free(praw); free(araw);
    if (npos < 1) { ds4_buf_puts(b, "error: recolor needs points: a few [x,y] on the thing to recolor\n"); return false; }
    if (framed) { ds4_buf_puts(b, "error: recolor points are pixels of the source image, so recolor must come before crop/resize/rotate/flip\n"); return false; }
    uint8_t *mask = img_job_mask(j, i, im); const char *how = mask ? "sam" : "color";
    if (!mask) mask = img_mask_color(im, pos, npos, neg, nneg);
    float *alpha = img_alpha_guided(im, mask, 8, 1e-3f); free(mask);
    const double share = img_alpha_share(alpha, (size_t)im->w * im->h); const img_rect bb = img_alpha_bbox(alpha, im->w, im->h);
    int on = 0; for (int i = 0; i < npos; i++) { const int x = pos[2 * i], y = pos[2 * i + 1]; if (x >= 0 && y >= 0 && x < im->w && y < im->h && alpha[(size_t)y * im->w + x] > 0.5f) on++; }
    if (!*overlay_done) {
        ds4_img ov = img_overlay(im, alpha, 0xff0000); char p[ASST_PATH + 24]; preview_path(j, "mask", p, sizeof p);
        if (write_preview(&ov, p, j->trash)) { char r[ASST_PATH]; rel(j, p, r, sizeof r); ds4_buf_printf(b, "image: %s\n", r); *overlay_done = true; }
        free(ov.rgb);
    }
    img_recolor(im, alpha, rgb); free(alpha);
    ds4_buf_printf(b, "recolor #%06x: mask from %s%s covers %.1f%% of the image (bbox x %d..%d, y %d..%d), %d of %d points inside the mask%s%s. The second image shows the mask in red. Judge from it, not from the shading: a recolored thing keeps its highlights and folds, so lighter streaks inside the red area are normal. Only call again if red clearly covers something that is not the thing (add one avoid point there) or clearly misses a part (add one point there); 2 to 6 points are enough, piling on points makes the mask worse.\n",
                   rgb, how, how[0] == 'c' && i < IMG_MAX_OPS ? j->seg[i].why : "", share * 100, bb.x, bb.x + bb.w - 1, bb.y, bb.y + bb.h - 1, on, npos, on < npos ? " (a point outside its own mask usually sits on an edge)" : "",
                   (npos > SEG_PTS || nneg > SEG_PTS) && how[0] == 's' ? " (only the first 8 points and 8 avoid points were used)" : "");
    if (how[0] == 's') img_job_seg_notes(j, i, b);
    return true;
}

/* 改动落点的并集(原图坐标), 给放大核对图定窗口 */
typedef struct { img_rect r; bool have; } focus_t;
static void focus_add(focus_t *f, img_rect r) {
    if (r.w <= 0 || r.h <= 0) return;
    if (!f->have) { f->r = r; f->have = true; return; }
    const int x1 = f->r.x + f->r.w > r.x + r.w ? f->r.x + f->r.w : r.x + r.w, y1 = f->r.y + f->r.h > r.y + r.h ? f->r.y + f->r.h : r.y + r.h;
    if (r.x < f->r.x) f->r.x = r.x; if (r.y < f->r.y) f->r.y = r.y;
    f->r.w = x1 - f->r.x; f->r.h = y1 - f->r.y;
}
/* 窗口 = 落点四周各扩一个落点长边(裁剪扩四分之一: 要看的是框边上切掉了什么, 框本身常占大半张图), 每边不小于原图长边的 ZOOM_MIN_SIDE,
 * 平移进图内。返回 false = 窗口快盖满整图(没必要再给一张; 裁剪除外, 整图上的压暗也能看出切掉了什么) */
static bool zoom_window(const ds4_img *im, img_rect f, bool force, img_rect *w) {
    const int pad = (f.w > f.h ? f.w : f.h) / (force ? 4 : 1), L = im->w > im->h ? im->w : im->h, mn = (int)(L * ZOOM_MIN_SIDE);
    int ww = f.w + 2 * pad, hh = f.h + 2 * pad; if (ww < mn) ww = mn; if (hh < mn) hh = mn;
    if (ww > im->w) ww = im->w; if (hh > im->h) hh = im->h;
    int x = f.x + f.w / 2 - ww / 2, y = f.y + f.h / 2 - hh / 2;
    if (x + ww > im->w) x = im->w - ww; if (y + hh > im->h) y = im->h - hh; if (x < 0) x = 0; if (y < 0) y = 0;
    *w = (img_rect){ x, y, ww, hh };
    return force || ww < im->w * 9 / 10 || hh < im->h * 9 / 10;
}

static bool frames(const char *nm) { return !strcmp(nm, "crop") || !strcmp(nm, "resize") || !strcmp(nm, "rotate") || !strcmp(nm, "flip"); }
static bool takes_points(const char *nm) { return !strcmp(nm, "crop") || !strcmp(nm, "draw") || !strcmp(nm, "pixelate") || !strcmp(nm, "blur") || !strcmp(nm, "adjust"); }
/* 抠出来的图给模型看: 垫棋盘格, 透明处一眼可见(垫白/垫黑会和白墙、黑衣服混在一起) */
static ds4_img on_checker(const ds4_img *im, const uint8_t *alpha) {
    ds4_img c = { im->w, im->h, malloc((size_t)im->w * im->h * 3) }; const int L = im->w > im->h ? im->w : im->h, cell = L / 48 > 8 ? L / 48 : 8;
    for (int y = 0; y < im->h; y++) for (int x = 0; x < im->w; x++) {
        const size_t k = (size_t)y * im->w + x; const float a = alpha[k] / 255.0f, bg = ((x / cell + y / cell) & 1) ? 200.0f : 140.0f;
        for (int ch = 0; ch < 3; ch++) c.rgb[k * 3 + ch] = (uint8_t)(im->rgb[k * 3 + ch] * a + bg * (1 - a) + 0.5f);
    }
    return c;
}

static char *image_child(void *ud) {
    const image_job *j = ud; ds4_buf b = {0}, log = {0}; char err[256]; ds4_img im;
    if (!img_load(j->path, &im, err, sizeof err)) { ds4_buf_printf(&b, "error: %s\n", err); return ds4_buf_take(&b); }
    const int w0 = im.w, h0 = im.h; bool overlay_done = false; int nops = 0;
    /* 核对图只看改变画幅(裁/缩/转/翻)之前的落点: 之后的坐标已不是原图的。ctx = 第一次改画幅前那一刻的图; 第一刀是裁剪就在核对图上框出裁剪框 */
    focus_t fo = {0}; ds4_img ctx = {0}; bool framed = false, cropped = false; img_rect crop_r = {0};
    uint8_t *alpha = NULL;   /* cutout 之后才有: 成品存 RGBA */
    for (int i = 0; ; i++) {
        char *op = NULL; if (!asst_jarr_at(j->ops, i, &op) || !op) break;
        char *name = NULL; asst_jstr(op, "op", &name); const char *nm = name ? name : "";
        if (i) ds4_buf_puts(&log, ", ");
        if (alpha) { ds4_buf_puts(&b, "error: cutout must be the last op; paste or resize the cut-out PNG in another call\n"); goto fail; }
        /* 带 points 的 op: 范围 = 分割出的东西的外接框。画椭圆外扩 21%: 外接椭圆的框是外接矩形的 √2 倍, 才整个套得住; 其余外扩 4% 留一点边 */
        img_rect pr = { 0, 0, 0, 0 }; bool pts = false;
        if (takes_points(nm)) {
            char *shp = NULL; asst_jstr(op, "shape", &shp); const float grow = !strcmp(nm, "draw") && shp && !strcmp(shp, "ellipse") ? 0.21f : 0.04f; free(shp);
            if (!img_points_region(j, i, op, nm, &im, framed, grow, &pr, &pts, &b)) goto fail;
        }
        const img_rect xywh = pts ? pr : (img_rect){ (int)num(op, "x", 0), (int)num(op, "y", 0), (int)num(op, "w", 0), (int)num(op, "h", 0) }, reg = pts ? pr : region_of(op);
        if (!framed && frames(nm)) {
            framed = true; ctx.w = im.w; ctx.h = im.h; ctx.rgb = malloc((size_t)im.w * im.h * 3); memcpy(ctx.rgb, im.rgb, (size_t)im.w * im.h * 3);
            if (!strcmp(nm, "crop")) { cropped = true; crop_r = xywh; focus_add(&fo, crop_r); }
        }
        if (!framed && (!strcmp(nm, "adjust") || !strcmp(nm, "blur") || !strcmp(nm, "pixelate"))) focus_add(&fo, reg);
        if (!strcmp(nm, "crop")) { if (!img_crop(&im, xywh)) { ds4_buf_puts(&b, "error: crop rectangle is empty or outside the image\n"); goto fail; } ds4_buf_printf(&log, "crop → %dx%d", im.w, im.h); }
        else if (!strcmp(nm, "resize")) { long tw = num(op, "width", 0), th = num(op, "height", 0); if (tw <= 0 && th <= 0) { ds4_buf_puts(&b, "error: resize needs width and/or height\n"); goto fail; }
            if (tw <= 0) tw = (long)((double)im.w * th / im.h + 0.5); if (th <= 0) th = (long)((double)im.h * tw / im.w + 0.5); img_resize(&im, (int)tw, (int)th); ds4_buf_printf(&log, "resize → %dx%d", im.w, im.h); }
        else if (!strcmp(nm, "rotate")) { const long d = num(op, "degrees", 90); if (d % 90) { ds4_buf_puts(&b, "error: rotate degrees must be 90, 180 or 270\n"); goto fail; } img_rotate(&im, (int)d); ds4_buf_printf(&log, "rotate %ld", d); }
        else if (!strcmp(nm, "flip")) { char *ax = NULL; asst_jstr(op, "axis", &ax); img_flip(&im, !ax || ax[0] != 'v'); ds4_buf_printf(&log, "flip %s", ax && ax[0] == 'v' ? "vertical" : "horizontal"); free(ax); }
        else if (!strcmp(nm, "adjust")) { img_adjust(&im, reg, (float)num(op, "brightness", 0), (float)num(op, "contrast", 0), (float)num(op, "saturation", 0)); ds4_buf_puts(&log, "adjust"); }
        else if (!strcmp(nm, "blur")) { img_blur(&im, reg, (int)num(op, "radius", 12)); ds4_buf_puts(&log, "blur"); }
        else if (!strcmp(nm, "pixelate")) { img_pixelate(&im, reg, (int)num(op, "block", 16)); ds4_buf_puts(&log, "pixelate"); }
        else if (!strcmp(nm, "draw")) {
            uint32_t rgb; if (!color_of(op, &rgb, &b)) goto fail;
            char *shape = NULL; asst_jstr(op, "shape", &shape); const char *sh = shape ? shape : "rect"; bool fill = false; asst_jbool(op, "fill", &fill); const int wd = (int)num(op, "width", 6);
            if (!strcmp(sh, "line") || !strcmp(sh, "arrow")) { const int x1 = (int)num(op, "x1", 0), y1 = (int)num(op, "y1", 0), x2 = (int)num(op, "x2", 0), y2 = (int)num(op, "y2", 0);
                img_draw_line(&im, x1, y1, x2, y2, rgb, wd, sh[0] == 'a'); if (!framed) focus_add(&fo, (img_rect){ (x1 < x2 ? x1 : x2) - wd, (y1 < y2 ? y1 : y2) - wd, abs(x2 - x1) + 2 * wd + 1, abs(y2 - y1) + 2 * wd + 1 }); }
            else { const img_rect r = xywh; if (!strcmp(sh, "ellipse")) img_draw_ellipse(&im, r, rgb, wd, fill); else img_draw_rect(&im, r, rgb, wd, fill); if (!framed) focus_add(&fo, r); }
            ds4_buf_printf(&log, "draw %s", sh); free(shape);
        }
        else if (!strcmp(nm, "paste")) {
            char *src = NULL; asst_jstr(op, "src", &src); char abs[ASST_PATH]; int sw, sh; uint8_t *rgba = NULL;
            if (!src || !asst_path_resolve(src, abs, sizeof abs) || !asst_under(abs, j->ws) || !img_load_rgba(abs, &sw, &sh, &rgba, err, sizeof err)) { ds4_buf_printf(&b, "error: paste src must be an image in the workspace (%s)\n", src ? src : "missing"); free(src); goto fail; }
            double op_ = 1; char *os = NULL; if (asst_jraw(op, "opacity", &os) && os) op_ = atof(os); free(os);
            const int px = (int)num(op, "x", 0), py = (int)num(op, "y", 0), tw = (int)num(op, "width", 0);
            img_paste(&im, rgba, sw, sh, px, py, tw, (float)op_); free(rgba); if (!framed) focus_add(&fo, (img_rect){ px, py, tw > 0 ? tw : sw, tw > 0 ? (int)((double)sh * tw / sw + 0.5) : sh }); ds4_buf_printf(&log, "paste %s", src); free(src);
        }
        else if (!strcmp(nm, "recolor")) { if (!recolor(j, i, op, &im, framed, &b, &overlay_done)) goto fail; ds4_buf_puts(&log, "recolor"); }
        else if (!strcmp(nm, "cutout")) { if (!img_cutout(j, i, op, &im, framed, &alpha, &b)) goto fail; ds4_buf_printf(&log, "cutout → %dx%d", im.w, im.h); }
        else { ds4_buf_printf(&b, "error: unknown op \"%s\" (crop, resize, rotate, flip, adjust, blur, pixelate, draw, paste, recolor, cutout)\n", nm); goto fail; }
        free(name); free(op); nops++; continue;
    fail:
        free(name); free(op); img_free(&im); img_free(&ctx); free(alpha); ds4_buf_free(&log);
        { const char *e = b.ptr ? strstr(b.ptr, "error:") : NULL; char *r = strdup(e ? e : "error: edit failed\n"); ds4_buf_free(&b); return r; }   /* 只回错误句: 前面成功的 op 的说明在前, 结果就不以 error 开头, 会被当成成功(10-15 单测撞出) */
    }
    if (!nops) { img_free(&im); img_free(&ctx); ds4_buf_free(&log); ds4_buf_puts(&b, "error: ops is empty\n"); return ds4_buf_take(&b); }
    size_t n; uint8_t *png;
    if (alpha) { uint8_t *rgba = malloc((size_t)im.w * im.h * 4); for (size_t k = 0; k < (size_t)im.w * im.h; k++) { memcpy(rgba + k * 4, im.rgb + k * 3, 3); rgba[k * 4 + 3] = alpha[k]; } png = img_png_encode(im.w, im.h, 4, rgba, &n); free(rgba); }
    else png = img_png_encode(im.w, im.h, 3, im.rgb, &n);
    char *werr = asst_fs_put_bytes(j->out, png, n, j->trash); free(png);
    if (werr) { img_free(&im); img_free(&ctx); free(alpha); ds4_buf_free(&log); ds4_buf_free(&b); return werr; }
    char pv[ASST_PATH + 24], pr[ASST_PATH]; preview_path(j, "preview", pv, sizeof pv); rel(j, pv, pr, sizeof pr);
    bool has_pv; if (alpha) { ds4_img c = on_checker(&im, alpha); has_pv = write_preview(&c, pv, j->trash); img_free(&c); } else has_pv = write_preview(&im, pv, j->trash);
    ds4_buf out = {0}; char r[ASST_PATH]; rel(j, j->out, r, sizeof r);
    ds4_buf_printf(&out, "image: %s\n", has_pv ? pr : r);
    const ds4_img *base = framed ? &ctx : &im; img_rect zw;
    if (fo.have && zoom_window(base, fo.r, cropped, &zw)) {
        ds4_img z = img_zoom_grid(base, zw, ZOOM_MAX, cropped ? &crop_r : NULL); char zp[ASST_PATH + 24], zr[ASST_PATH]; preview_path(j, "zoom", zp, sizeof zp);
        size_t zn = 0; uint8_t *zpng = z.rgb ? img_png_encode(z.w, z.h, 3, z.rgb, &zn) : NULL; char *zerr = zpng ? asst_fs_put_bytes(zp, zpng, zn, j->trash) : strdup("no zoom"); free(zpng); img_free(&z);
        if (!zerr) { rel(j, zp, zr, sizeof zr); ds4_buf_printf(&out, "image: %s\n", zr);
            ds4_buf_printf(&b, "The zoom image shows x %d..%d, y %d..%d of the %s with a magenta grid: the numbers at each grid crossing are that point's x,y in source pixels%s. Check the edit against what it was meant to cover there, not in the small full view. If it is off, read where the thing really is from the grid numbers and call again with corrected coordinates.\n",
                           zw.x, zw.x + zw.w - 1, zw.y, zw.y + zw.h - 1, framed ? "image before cropping/resizing" : "result", cropped ? "; the bright part inside the white box is what the crop keeps, the dimmed part is cut away. A rectangular crop always keeps some background around the thing; that is expected" : ""); }
        free(zerr);
    }
    img_free(&im); img_free(&ctx); ds4_buf_append(&out, b.ptr, b.len);
    ds4_buf_printf(&out, "saved %s: %dx%d PNG%s, %zu KB (source %dx%d); ops: %s. Write the path in your answer and the user can download it.\n", r, im.w, im.h, alpha ? " with transparency" : "", (n + 1023) / 1024, w0, h0, log.ptr ? log.ptr : "");
    free(alpha); ds4_buf_free(&b); ds4_buf_free(&log); return ds4_buf_take(&out);
}

char *asst_image_exec(const char *args, const asst_gres *g, const char *trash, int *ok) {
    *ok = 0;
    char *ops = NULL, *outp = NULL; asst_jraw(args, "ops", &ops); asst_jstr(args, "out", &outp);
    if (!ops || ops[0] != '[') { free(ops); free(outp); return strdup("error: ops must be an array of operations\n"); }
    static char out[ASST_PATH + 16];
    if (outp && outp[0]) { if (!asst_path_resolve(outp, out, sizeof out) || !asst_under(out, asst_ws)) { free(ops); free(outp); return strdup("error: out must be a path inside the workspace\n"); } }
    else { const char *dot = strrchr(g->path, '.'), *sl = strrchr(g->path, '/'); const int stem = dot && (!sl || dot > sl) ? (int)(dot - g->path) : (int)strlen(g->path); snprintf(out, sizeof out, "%.*s-edited.png", stem, g->path); }
    const char *oe = strrchr(out, '.'); if (!oe || strcasecmp(oe, ".png")) { free(ops); free(outp); return strdup("error: out must end in .png (the editor writes PNG)\n"); }
    static image_job j; memset(&j, 0, sizeof j); j.path = g->path; j.out = out; j.ops = ops; j.trash = trash; snprintf(j.ws, sizeof j.ws, "%s", asst_ws);   /* static: 每个 op 一份掩膜路径, 上百 KB 不放栈上 */
    img_job_segment(&j);   /* 带 points 的 op 先在主进程跑分割(tool_image_seg.c): 没有分割模型时 recolor 退颜色流水线, 其余 op 报错让模型改给坐标 */
    asst_sbx_spec sp; asst_guard_spec(g, false, &sp); sp.timeout_s = 120;
    char *res = NULL; const asst_sbx_rc rc = asst_sbx_run(&sp, image_child, &j, &res);
    free(ops); free(outp);
    if (rc == ASST_SBX_LOCK_FAILED) { ds4_buf b = {0}; ds4_buf_printf(&b, "error: refused to run without a sandbox: %s\n", res ? res : ""); free(res); return ds4_buf_take(&b); }
    *ok = rc == ASST_SBX_OK && res && strncmp(res, "error", 5) != 0;
    return res ? res : strdup("error: no result from the image editor\n");
}

/* view_image: 只看不改 —— 一块放大到长边 ZOOM_MAX + 原图坐标网格(img_grid.c), 落 .img/<名>.view-x-y-w-h.png(按区域命名: 历史里每次看的图各是各的)。
 * 为什么单独一个工具(10-15 真模型门, 熊图打码): 模型要先看清小东西在哪再动手, 没有它就拿 crop+resize 当放大镜, 9 次调用里 6 次是"看", 还在工作区留下一串 zoom-*.png */
typedef struct { const char *path; img_rect r; char ws[ASST_PATH]; const char *trash; } view_job;
static char *view_child(void *ud) {
    const view_job *v = ud; ds4_buf b = {0}; char err[256]; ds4_img im; uint8_t *rgba = NULL;
    /* 按 RGBA 读: 抠出来的图透明处原样是背景色, 只读 RGB 会让它看起来没抠(10-15 真模型门: 模型因此去写 Python 数透明像素); 有透明就垫棋盘格 */
    if (!img_load_rgba(v->path, &im.w, &im.h, &rgba, err, sizeof err)) { ds4_buf_printf(&b, "error: %s\n", err); return ds4_buf_take(&b); }
    const size_t np = (size_t)im.w * im.h; im.rgb = malloc(np * 3); uint8_t *al = malloc(np); bool any = false;
    for (size_t k = 0; k < np; k++) { memcpy(im.rgb + k * 3, rgba + k * 4, 3); al[k] = rgba[k * 4 + 3]; any |= al[k] < 255; }
    free(rgba);
    if (any) { ds4_img c = on_checker(&im, al); img_free(&im); im = c; }
    free(al);
    img_rect r = v->r; if (r.w <= 0 || r.h <= 0) r = (img_rect){ 0, 0, im.w, im.h };
    const int mn = (int)((im.w > im.h ? im.w : im.h) * VIEW_MIN_SIDE);
    if (r.w < mn) { r.x -= (mn - r.w) / 2; r.w = mn; if (r.x < 0) r.x = 0; if (r.x + r.w > im.w) r.x = im.w - r.w > 0 ? im.w - r.w : 0; }   /* 扩大的窗口贴边时平移进图, 不缩回去 */
    if (r.h < mn) { r.y -= (mn - r.h) / 2; r.h = mn; if (r.y < 0) r.y = 0; if (r.y + r.h > im.h) r.y = im.h - r.h > 0 ? im.h - r.h : 0; }
    if (r.x < 0) { r.w += r.x; r.x = 0; } if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > im.w) r.w = im.w - r.x; if (r.y + r.h > im.h) r.h = im.h - r.y;
    if (r.w < 4 || r.h < 4 || (v->r.w > 0 && (v->r.x >= im.w || v->r.y >= im.h || v->r.x + v->r.w <= 0 || v->r.y + v->r.h <= 0))) { ds4_buf_printf(&b, "error: the region is outside the %dx%d image\n", im.w, im.h); img_free(&im); return ds4_buf_take(&b); }
    ds4_img z = img_zoom_grid(&im, r, ZOOM_MAX, NULL); const int W = im.w, H = im.h; img_free(&im);
    const char *sl = strrchr(v->path, '/'), *base = sl ? sl + 1 : v->path, *dot = strrchr(base, '.');
    char p[ASST_PATH + 96]; snprintf(p, sizeof p, "%s/.img/%.*s.view-%d-%d-%d-%d.png", v->ws, (int)(dot ? dot - base : (long)strlen(base)), base, r.x, r.y, r.w, r.h);
    size_t n; uint8_t *png = img_png_encode(z.w, z.h, 3, z.rgb, &n); char *werr = asst_fs_put_bytes(p, png, n, v->trash); free(png); img_free(&z);
    if (werr) return werr;
    ds4_buf_printf(&b, "image: %s\nThis is x %d..%d, y %d..%d of the %dx%d source, enlarged%s. The numbers at each magenta grid crossing are that point's x,y in source pixels: read positions off them and use them as-is in edit_image.\n",
                   p + strlen(v->ws) + 1, r.x, r.x + r.w - 1, r.y, r.y + r.h - 1, W, H, any ? "; the checkerboard shows transparent parts" : "");
    return ds4_buf_take(&b);
}
char *asst_image_view(const char *args, const asst_gres *g, const char *trash, int *ok) {
    view_job v = { g->path, { (int)num(args, "x", 0), (int)num(args, "y", 0), (int)num(args, "w", 0), (int)num(args, "h", 0) }, "", trash };
    snprintf(v.ws, sizeof v.ws, "%s", asst_ws);
    char d[ASST_PATH + 8]; snprintf(d, sizeof d, "%s/.img", asst_ws); mkdir(d, 0700);
    asst_sbx_spec sp; asst_guard_spec(g, false, &sp); sp.timeout_s = 60;
    char *res = NULL; const asst_sbx_rc rc = asst_sbx_run(&sp, view_child, &v, &res);
    if (rc == ASST_SBX_LOCK_FAILED) { ds4_buf b = {0}; ds4_buf_printf(&b, "error: refused to run without a sandbox: %s\n", res ? res : ""); free(res); return ds4_buf_take(&b); }
    *ok = rc == ASST_SBX_OK && res && strncmp(res, "error", 5) != 0;
    return res ? res : strdup("error: no result from the image viewer\n");
}
