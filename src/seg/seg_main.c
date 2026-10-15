/* seg_main.c — ds4-seg: 点提示分割命令行(改图工具在主进程里 spawn 它, 进程退出 = 权重与 GPU 全释放; docs/assistant_image.md §3.6)。
 *   ds4-seg -m gguf/seg/mobile_sam.pt -i 图.jpg -o 掩膜.png -p x,y[,x,y…] [-n x,y[,x,y…]]
 * -p = 要的东西上的点, -n = 不要的东西上的点(原图像素坐标)。出: 8 位灰度 PNG, 值 = sigmoid(logit)·255(≥128 即模型判在内, 软边给导向滤波用),
 * 以及 stdout 一行 JSON: {"iou":…,"w":…,"h":…,"inside":占比,"dropped_pos":[被丢的正点],"dropped_neg":[贴边被丢的负点],"device":…,"ms":…}。退出码 0 成功; 任何错 → stderr 一行 + 退出 1。 */
#include "ds4_seg.h"
#include "seg_ops.h"
#include "../image/ds4_image.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static int parse_points(const char *s, float *pts, int *labels, int label, int n) {
    while (s && *s && n < SEG_MAX_POINTS) {
        char *e; const double x = strtod(s, &e); if (e == s || *e != ',') return -1; s = e + 1;
        const double y = strtod(s, &e); if (e == s) return -1; s = e;
        pts[2 * n] = (float)x; pts[2 * n + 1] = (float)y; labels[n++] = label;
        if (*s == ',') s++; else if (*s) return -1;
    }
    return n;
}

int main(int argc, char **argv) {
    const char *model = NULL, *in = NULL, *out = NULL, *pos = NULL, *neg = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc) model = argv[++i];
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) in = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) pos = argv[++i];
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) neg = argv[++i];
        else { fprintf(stderr, "usage: ds4-seg -m weights.pt -i image -o mask.png -p x,y[,x,y...] [-n x,y[,x,y...]]\n"); return 1; }
    }
    if (!model || !in || !out || !pos) { fprintf(stderr, "ds4-seg: -m, -i, -o and -p are required\n"); return 1; }
    float pts[SEG_MAX_POINTS * 2]; int labels[SEG_MAX_POINTS];
    int n = parse_points(pos, pts, labels, 1, 0); if (n > 0 && neg) n = parse_points(neg, pts, labels, 0, n);
    if (n <= 0) { fprintf(stderr, "ds4-seg: bad point list (want x,y,x,y...)\n"); return 1; }
    const double t0 = now_ms(); char err[512] = "";
    ds4_img im;
    if (!img_load(in, &im, err, sizeof err)) { fprintf(stderr, "ds4-seg: %s\n", err); return 1; }
    if (!seg_dev_init(err, sizeof err)) { fprintf(stderr, "ds4-seg: %s\n", err); return 1; }
    seg_pt *pt = seg_pt_open(model, err, sizeof err); if (!pt) { fprintf(stderr, "ds4-seg: %s\n", err); return 1; }
    seg_model *m = seg_model_load(pt, err, sizeof err); if (!m) { fprintf(stderr, "ds4-seg: %s\n", err); return 1; }
    const double t1 = now_ms();
    float *mask = NULL, iou = 0; int kept[SEG_MAX_POINTS];
    if (!seg_run(m, im.rgb, im.w, im.h, pts, labels, n, &mask, &iou, kept, err, sizeof err)) { fprintf(stderr, "ds4-seg: %s\n", err); return 1; }
    const double t2 = now_ms();
    uint8_t *g = malloc((size_t)im.w * im.h); size_t inside = 0;
    for (size_t i = 0; i < (size_t)im.w * im.h; i++) { const float s = 1.0f / (1.0f + expf(-mask[i])); g[i] = (uint8_t)(s * 255.0f + 0.5f); inside += mask[i] > 0; }
    if (!img_png_write(out, im.w, im.h, 1, g)) { fprintf(stderr, "ds4-seg: cannot write %s\n", out); return 1; }
    printf("{\"iou\":%.4f,\"w\":%d,\"h\":%d,\"inside\":%.4f,\"points\":%d,\"dropped_pos\":[", iou, im.w, im.h, (double)inside / ((double)im.w * im.h), n);
    int k = 0; for (int i = 0; i < n; i++) if (!kept[i] && labels[i]) printf("%s[%d,%d]", k++ ? "," : "", (int)pts[2 * i], (int)pts[2 * i + 1]);
    printf("],\"dropped_neg\":["); k = 0; for (int i = 0; i < n; i++) if (!kept[i] && !labels[i]) printf("%s[%d,%d]", k++ ? "," : "", (int)pts[2 * i], (int)pts[2 * i + 1]);
    printf("],\"device\":\"%s\",\"load_ms\":%.0f,\"run_ms\":%.0f}\n", seg_dev_name(), t1 - t0, t2 - t1);
    free(g); free(mask); img_free(&im);
    seg_model_free(m); seg_pt_close(pt); seg_dev_free();
    return 0;
}
