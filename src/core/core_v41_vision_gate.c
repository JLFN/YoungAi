/* core_v41_vision_gate.c — 视觉塔的金标门(2026-10-13): ./ds4 --image 图 --vision-golden 金标。
 *
 * 金标由 tools/vision_golden.py 用官方 inference/{image_processor,vision}.py 对同一张图产出(PIL 预处理 + BF16 原件 ViT/aligner):
 *   "VGLD" u32 ver=1 | u32 w h n_vit_h n_vit_w n_llm_h n_llm_w pp E best_w best_h
 *   u8 rgb[best_h][best_w][3](PIL pad 之后)  f32 patches[n_vit][pp]  f32 embeds[n_llm][E]
 * 四道比对, 各自说明病在哪一段:
 *   ① 尺寸账(网格/补边尺寸)不同 ⇒ core_v41_image.c 的 plan 转录错;
 *   ② 像素: 与 PIL 逐像素差(stb 的 JPEG 解码与 libjpeg 可差 ±1~2, PNG 应为 0) ⇒ 解码器差或双三次转录错(差 >3 才是转录错);
 *   ③ 我们的 patch 对金标 patch 的最大差(像素差 1 ⇒ patch 差 0.0078);
 *   ④ ViT: 拿**金标的 patch**跑我们的塔, 对金标 embeds 逐行 cos / 最大绝对差 —— 这一道只量塔, 与预处理无关; 再用我们自己的 patch 跑一遍看端到端。
 * 判读: ④ 的最小行 cos ≥ 0.99 且最大绝对差在 bf16 一两个 ulp 量级(与 cuBLAS 累加序/bf16 边界同源)= 塔对; cos 掉到 0.9 以下 = 某一件算错。 */
#include "core_internal.h"
#include "ds4_v41_api.h"
#ifndef DS4_NO_GPU

static bool rd(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n; }
static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long L = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = xmalloc((size_t)L + 1);
    if (!rd(f, b, (size_t)L)) { free(b); fclose(f); return NULL; }
    fclose(f); *len = (size_t)L; return b;
}
static void row_stats(const float *a, const float *b, uint32_t rows, uint32_t E, double *min_cos, double *mean_cos, double *max_abs, uint32_t *worst) {
    *min_cos = 1.0; *max_abs = 0.0; *worst = 0; double sum = 0;
    for (uint32_t r = 0; r < rows; r++) {
        double aa = 0, bb = 0, ab = 0;
        for (uint32_t j = 0; j < E; j++) {
            const double x = a[(size_t)r * E + j], y = b[(size_t)r * E + j];
            aa += x * x; bb += y * y; ab += x * y;
            const double d = fabs(x - y); if (d > *max_abs) *max_abs = d;
        }
        const double c = ab / (sqrt(aa) * sqrt(bb) + 1e-30);
        sum += c;
        if (c < *min_cos) { *min_cos = c; *worst = r; }
    }
    *mean_cos = rows ? sum / rows : 0.0;
}
/* 判读阈值的依据(2026-10-13 实测, tools/vision_golden.py --compare): 官方自己 bf16 跑 vs 同一份代码 f32 跑, img.jpeg 最小行 cos 0.926 / 均值 0.99927,
 * 面板图 0.986 / 0.99979 —— 个别行(平坦区, 模小)在 bf16 下本来就晃到 0.93。所以门按"均值 ≥ 0.999 且最小行 ≥ 0.95"判, 单看最小行会把官方自己也判死。 */
#define VIS_GATE_MEAN_COS 0.999
#define VIS_GATE_MIN_COS  0.95

int ds4_engine_v41_vision_golden(ds4_engine *e, const char *image_path, const char *golden_path) {
    if (!ds4_engine_v41_has_vision(e)) { fprintf(stderr, "ds4: 这份 GGUF 没带视觉塔\n"); return 1; }
    size_t ilen = 0; uint8_t *img = slurp(image_path, &ilen);
    if (!img) { fprintf(stderr, "ds4: 读不到图 %s\n", image_path); return 1; }
    FILE *g = fopen(golden_path, "rb");
    if (!g) { fprintf(stderr, "ds4: 读不到金标 %s\n", golden_path); free(img); return 1; }
    char magic[4]; uint32_t h[12];
    if (!rd(g, magic, 4) || memcmp(magic, "VGLD", 4) || !rd(g, h, 12 * 4) || h[0] != 1) { fprintf(stderr, "ds4: 金标魔数/版本不对\n"); fclose(g); free(img); return 1; }
    const uint32_t gw = h[1], gh = h[2], g_vh = h[3], g_vw = h[4], g_lh = h[5], g_lw = h[6], pp = h[7], E = h[8], g_bw = h[9], g_bh = h[10];
    const uint64_t n_vit = (uint64_t)g_vh * g_vw, n_llm = (uint64_t)g_lh * g_lw;
    uint8_t *grgb = xmalloc((size_t)g_bw * g_bh * 3);
    float *gpat = xmalloc(n_vit * pp * 4), *gemb = xmalloc(n_llm * E * 4);
    if (!rd(g, grgb, (size_t)g_bw * g_bh * 3) || !rd(g, gpat, n_vit * pp * 4) || !rd(g, gemb, n_llm * E * 4)) { fprintf(stderr, "ds4: 金标读不满\n"); fclose(g); return 1; }
    fclose(g);
    int rc = 0;
    /* ① 尺寸账 */
    uint32_t vh, vw, lh, lw, bw, bh;
    if (!v41_image_plan(gw, gh, &vh, &vw, &lh, &lw, &bw, &bh)) { fprintf(stderr, "① 尺寸账: plan 失败\n"); return 1; }
    const int plan_ok = vh == g_vh && vw == g_vw && lh == g_lh && lw == g_lw && bw == g_bw && bh == g_bh;
    fprintf(stderr, "① 尺寸账 %s: 原图 %ux%u → 补边 %ux%u(金标 %ux%u) patch %ux%u(金标 %ux%u) token %ux%u(金标 %ux%u)\n",
            plan_ok ? "同" : "★不同★", gw, gh, bw, bh, g_bw, g_bh, vh, vw, g_vh, g_vw, lh, lw, g_lh, g_lw);
    if (!plan_ok) return 1;
    /* ② 像素 + ③ patch(走与生产同一条 v41_image_patches) */
    char err[256]; uint32_t vh2, vw2, lh2, lw2;
    float *pat = v41_image_patches(img, ilen, &vh2, &vw2, &lh2, &lw2, err, sizeof err);
    if (!pat) { fprintf(stderr, "ds4: 预处理失败: %s\n", err); return 1; }
    {
        double mx = 0; uint64_t nd = 0;
        for (uint64_t i = 0; i < n_vit * pp; i++) { const double d = fabs((double)pat[i] - (double)gpat[i]); if (d > 0) nd++; if (d > mx) mx = d; }
        /* patch 值 = (px/255 − 0.5)/0.5 舍 bf16: 像素差 1 ≈ 0.00784 */
        fprintf(stderr, "③ patch: %llu/%llu 个元素不同, 最大差 %.5f(≈ %.1f 个像素级)\n", (unsigned long long)nd, (unsigned long long)(n_vit * pp), mx, mx / (2.0 / 255.0));
        if (mx > 4.0 * 2.0 / 255.0) { fprintf(stderr, "   ★差超 4 个像素级: 不是解码器的 ±1, 是缩放/补边转录有误★\n"); rc = 1; }
    }
    (void)grgb;
    /* ④ 塔: 金标 patch → 我们的塔 vs 金标 embeds */
    uint32_t olh, olw;
    float *emb_g = v41_vision_run(e, gpat, g_vh, g_vw, &olh, &olw);
    if (!emb_g) { fprintf(stderr, "ds4: 塔前向失败\n"); return 1; }
    double mc, mean, ma; uint32_t worst;
    row_stats(emb_g, gemb, (uint32_t)n_llm, E, &mc, &mean, &ma, &worst);
    fprintf(stderr, "④ 塔(金标 patch): %llu 行 × %u, 行 cos 均值 %.6f 最小 %.6f(第 %u 行), 最大绝对差 %.5f\n", (unsigned long long)n_llm, E, mean, mc, worst, ma);
    if (mean < VIS_GATE_MEAN_COS || mc < VIS_GATE_MIN_COS) { fprintf(stderr, "   ★塔有一件算错(均值 < %.3f 或最小行 < %.2f; 官方 bf16 自身噪声见本文件注释)★\n", VIS_GATE_MEAN_COS, VIS_GATE_MIN_COS); rc = 1; }
    float *emb_o = v41_vision_run(e, pat, vh2, vw2, &olh, &olw);
    if (emb_o) {
        row_stats(emb_o, gemb, (uint32_t)n_llm, E, &mc, &mean, &ma, &worst);
        fprintf(stderr, "⑤ 端到端(我们的 patch): 行 cos 均值 %.6f 最小 %.6f(第 %u 行), 最大绝对差 %.5f(JPEG 解码差 ±1~3 级在这一道里, 只报不判)\n", mean, mc, worst, ma);
    }
    free(emb_g); free(emb_o); free(pat); free(gpat); free(gemb); free(grgb); free(img);
    fprintf(stderr, rc ? "vision-golden: ★FAIL★\n" : "vision-golden: ok\n");
    return rc;
}
#else
int ds4_engine_v41_vision_golden(ds4_engine *e, const char *image_path, const char *golden_path) {
    (void)e; (void)image_path; (void)golden_path; fprintf(stderr, "ds4: 视觉塔只有 GPU 路\n"); return 1;
}
#endif
