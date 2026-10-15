/* seg_run.c — 一次分割的全流程(predictor.py 的 set_image + predict 两步合一):
 * 原图 → 长边缩到 1024(PIL 双线性) → 减均值除方差 → 右下补零到 1024² → 编码器 → 点坐标按同一比例换帧 → 解码器(无 mask 提示) →
 * 选掩膜(≥2 个点取单掩膜槽 0; 1 个点三掩膜里 IoU 最高) → 把它的低分辨率 logit 当 mask 提示再解码一轮(官方推荐的 refine) →
 * 选同一规则 → logit 256² → 双线性到 1024² → 裁掉补边 → 双线性到原图尺寸。>0 为内。 */
#include "seg_internal.h"
#include "../image/ds4_image.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const float MEAN[3] = { 123.675f, 116.28f, 103.53f }, STD[3] = { 58.395f, 57.12f, 57.375f };   /* 官方 pixel_mean / pixel_std */

static int pick(int npts, const float iou[4]) {
    if (npts >= 2) return 0;
    int best = 1; for (int i = 2; i < 4; i++) if (iou[i] > iou[best]) best = i;
    return best;
}

/* 点筛(10-15 真模型门实撞): 模型在东西边沿附近放点误差 ~90 px —— "避开背景"的负点放到帽顶上方, SAM 削帽顶; "帽顶"的正点放到帽子外, SAM 又把背景拉进来, 十轮不收。
 * 先只用正点解一次得 M: 负点若在 M 外且离 M 不到 NEG_MARGIN(256 格的 8%)丢掉(SAM 本来没算那儿, 留着只削边; M 里的负点是真修正, 远的无害);
 * 正点若落在 M 外(其余正点都同意的掩膜不含它)丢掉, 再解一次复核, 最多两轮。丢了的点数与坐标报给工具, 工具说给模型听, 不让它再加回去。 */
#define NEG_MARGIN 20   /* 256 格的 8%: 实测误差 13 格, 10 格拦不住 */
static void decode_pos(seg_model *m, seg_buf *emb, const float *sp, const int *label, const int *keep, int npts, float *lr) {
    float pp[SEG_MAX_POINTS * 2]; int pl[SEG_MAX_POINTS], np = 0;
    for (int i = 0; i < npts; i++) if (label[i] && keep[i]) { pp[2 * np] = sp[2 * i]; pp[2 * np + 1] = sp[2 * i + 1]; pl[np++] = 1; }
    float io[4]; seg_buf *low; seg_decode(m, emb, pp, pl, np, NULL, 0, &low, io);
    seg_buf_read(low, (size_t)pick(np, io) * SEG_LOWRES * SEG_LOWRES, lr, (size_t)SEG_LOWRES * SEG_LOWRES);
}
static bool cell_of(const float *sp, int i, int *gx, int *gy) { *gx = (int)(sp[2 * i] * SEG_LOWRES / SEG_IMG); *gy = (int)(sp[2 * i + 1] * SEG_LOWRES / SEG_IMG); return *gx >= 0 && *gy >= 0 && *gx < SEG_LOWRES && *gy < SEG_LOWRES; }
static void filter_points(seg_model *m, seg_buf *emb, const float *sp, const int *label, int npts, int *keep, int *neg_dropped, int *pos_dropped) {
    int np = 0; for (int i = 0; i < npts; i++) { keep[i] = 1; np += label[i] != 0; }
    *neg_dropped = *pos_dropped = 0; if (np < 2 && npts == np) return;
    float *lr = malloc(sizeof(float) * SEG_LOWRES * SEG_LOWRES);
    for (int round = 0; round < 2; round++) {
        decode_pos(m, emb, sp, label, keep, npts, lr); int changed = 0;
        for (int i = 0; i < npts; i++) {
            int gx, gy; if (!keep[i] || !cell_of(sp, i, &gx, &gy)) continue;
            const bool inside = lr[gy * SEG_LOWRES + gx] > 0;
            if (label[i]) { if (!inside && np > 1) { keep[i] = 0; np--; (*pos_dropped)++; changed++; } continue; }   /* 正点不在"其余正点同意"的掩膜里: 落到了背景/边沿 */
            if (inside || round) continue;   /* 负点只在第一轮筛: 在 M 里 = 真修正 */
            bool near = false;
            for (int y = gy - NEG_MARGIN; y <= gy + NEG_MARGIN && !near; y++) for (int x = gx - NEG_MARGIN; x <= gx + NEG_MARGIN; x++) if (x >= 0 && y >= 0 && x < SEG_LOWRES && y < SEG_LOWRES && lr[y * SEG_LOWRES + x] > 0) { near = true; break; }
            if (near) { keep[i] = 0; (*neg_dropped)++; }
        }
        if (!changed) break;
    }
    free(lr);
}

bool seg_run(seg_model *m, const uint8_t *rgb, int w, int h, const float *pts, const int *label, int npts, float **mask, float *iou, int *kept, char *err, size_t errn) {
    if (npts < 1 || npts > SEG_MAX_POINTS) { snprintf(err, errn, "need 1..%d points, got %d", SEG_MAX_POINTS, npts); return false; }
    if (w < 2 || h < 2) { snprintf(err, errn, "image too small"); return false; }
    int tw, th; seg_preprocess_shape(w, h, &tw, &th);
    uint8_t *rs = img_resize_ch(rgb, w, h, 3, tw, th);   /* PIL 双线性抗锯齿, 与改图工具同一份 */
    float *img = calloc((size_t)SEG_IMG * SEG_IMG * 3, sizeof(float));
    for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++) for (int c = 0; c < 3; c++) img[((size_t)y * SEG_IMG + x) * 3 + c] = (rs[((size_t)y * tw + x) * 3 + c] - MEAN[c]) / STD[c];
    free(rs);
    seg_buf *dimg = seg_buf_from(img, (size_t)SEG_IMG * SEG_IMG * 3); free(img);
    seg_buf *emb = seg_encode(m, dimg);
    seg_sync(); seg_buf_free(dimg);
    float sp0[SEG_MAX_POINTS * 2], sp[SEG_MAX_POINTS * 2]; int lab[SEG_MAX_POINTS], keep[SEG_MAX_POINTS], n = 0;
    for (int i = 0; i < npts; i++) { sp0[2 * i] = pts[2 * i] * ((float)tw / w); sp0[2 * i + 1] = pts[2 * i + 1] * ((float)th / h); }
    int nd, pd; filter_points(m, emb, sp0, label, npts, keep, &nd, &pd);
    for (int i = 0; i < npts; i++) { if (kept) kept[i] = keep[i]; if (keep[i]) { sp[2 * n] = sp0[2 * i]; sp[2 * n + 1] = sp0[2 * i + 1]; lab[n++] = label[i]; } }
    npts = n; label = lab;
    float io[4]; seg_buf *low;
    seg_decode(m, emb, sp, label, npts, NULL, 0, &low, io);
    int k = pick(npts, io);
    seg_buf *prev = seg_buf_new((size_t)SEG_LOWRES * SEG_LOWRES);
    seg_buf_copy(prev, 0, low, (size_t)k * SEG_LOWRES * SEG_LOWRES, (size_t)SEG_LOWRES * SEG_LOWRES);
    seg_decode(m, emb, sp, label, npts, prev, 0, &low, io);
    k = pick(npts, io);
    float *lr = malloc(sizeof(float) * SEG_LOWRES * SEG_LOWRES);
    seg_buf_read(low, (size_t)k * SEG_LOWRES * SEG_LOWRES, lr, (size_t)SEG_LOWRES * SEG_LOWRES);
    seg_buf_free(prev);
    for (int i = 0; i < SEG_LOWRES * SEG_LOWRES; i++) if (!isfinite(lr[i])) { free(lr); snprintf(err, errn, "mask logits not finite"); return false; }
    float *up = seg_resize_f32(lr, SEG_LOWRES, SEG_LOWRES, SEG_IMG, SEG_IMG); free(lr);
    float *crop = malloc(sizeof(float) * (size_t)tw * th);
    for (int y = 0; y < th; y++) memcpy(crop + (size_t)y * tw, up + (size_t)y * SEG_IMG, sizeof(float) * (size_t)tw);
    free(up);
    *mask = seg_resize_f32(crop, tw, th, w, h); free(crop);
    *iou = io[k];
    return true;
}
