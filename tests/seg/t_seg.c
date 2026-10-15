/* t_seg.c — 分割模型单测(make ds4_seg_test && ./ds4_seg_test [权重.pt] [图]): 要 GPU 设备; 权重缺就只跑算子项并 SKIP 模型项。
 * ① .pt 读取: 439 个 storage 里 FloatStorage 张量 412 个、形状对得上官方结构(patch_embed 32×3×3×3, neck 256×320×1×1, 10,140,204 个浮点参数)。
 * ② 算子: 每个 GPU 核与同一公式的标量算法在小尺寸上逐元素对拍(GEMM/im2col/dwconv/LN/attention/window/pixel-shuffle)。
 * ③ 模型: 编码器输出有限、解码器 IoU 头在真图 + 点上给出 >0.5; 掩膜落 --out 看。 */
#include "../../src/image/ds4_image.h"
#include "../../src/seg/ds4_seg.h"
#include "../../src/seg/seg_ops.h"
#include "../../src/seg/seg_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, passes;
#define CHECK(cond, ...) do { if (cond) passes++; else { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_pt(const char *path) {
    char err[256] = "";
    seg_pt *p = seg_pt_open(path, err, sizeof err);
    CHECK(p != NULL, "open %s: %s", path, err);
    if (!p) return;
    CHECK(seg_pt_count(p) == 412, "float tensors %d (want 412 = 439 storages - 27 long scalars)", seg_pt_count(p));
    size_t params = 0; for (int i = 0; i < seg_pt_count(p); i++) params += seg_pt_at(p, i)->numel;
    CHECK(params == 10140204, "params %zu (want 10,140,204)", params);
    const seg_tensor *t = seg_pt_find(p, "image_encoder.patch_embed.seq.0.c.weight");
    CHECK(t && t->ndim == 4 && t->shape[0] == 32 && t->shape[1] == 3 && t->shape[2] == 3 && t->shape[3] == 3, "patch_embed conv shape");
    CHECK(t && isfinite(t->data[0]) && fabsf(t->data[0]) < 10, "patch_embed data finite: %g", t ? t->data[0] : 0);
    t = seg_pt_find(p, "image_encoder.neck.0.weight");
    CHECK(t && t->shape[0] == 256 && t->shape[1] == 320, "neck shape");
    t = seg_pt_find(p, "mask_decoder.iou_prediction_head.layers.2.bias");
    CHECK(t && t->ndim == 1 && t->shape[0] == 4, "iou head bias shape");
    CHECK(seg_pt_find(p, "image_encoder.patch_embed.seq.0.bn.num_batches_tracked") == NULL, "long tensors skipped");
    seg_pt_close(p);
}

int seg_ops_fails, seg_ops_passes;   /* t_seg_ops.c */
void seg_ops_tests(void);

/* 真模型 + 真图: 编码器输出有限; 低分辨率 logit 不许有奇偶列/行交替(ConvTranspose 像素重排错位的指纹); IoU 头 > 0.5 */
static void test_model(const char *ptpath, const char *image) {
    char err[256] = ""; ds4_img img0;
    if (!img_load(image, &img0, err, sizeof err)) { printf("SKIP model: %s\n", err); return; }
    const int w = img0.w, h = img0.h; const uint8_t *rgb = img0.rgb;
    seg_pt *pt = seg_pt_open(ptpath, err, sizeof err); seg_model *m = pt ? seg_model_load(pt, err, sizeof err) : NULL;
    CHECK(m != NULL, "model load: %s", err);
    if (!m) return;
    int tw, th; seg_preprocess_shape(w, h, &tw, &th);
    uint8_t *rs = img_resize_ch(rgb, w, h, 3, tw, th); float *img = calloc((size_t)SEG_IMG * SEG_IMG * 3, 4);
    const float MEAN[3] = { 123.675f, 116.28f, 103.53f }, STD[3] = { 58.395f, 57.12f, 57.375f };
    for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++) for (int c = 0; c < 3; c++) img[((size_t)y * SEG_IMG + x) * 3 + c] = (rs[((size_t)y * tw + x) * 3 + c] - MEAN[c]) / STD[c];
    seg_buf *dimg = seg_buf_from(img, (size_t)SEG_IMG * SEG_IMG * 3);
    seg_buf *emb = seg_encode(m, dimg);
    float *e = malloc(4096 * 256 * 4); seg_buf_read(emb, 0, e, 4096 * 256);
    double s = 0, s2 = 0; int fin = 1; for (int i = 0; i < 4096 * 256; i++) { if (!isfinite(e[i])) fin = 0; s += e[i]; s2 += (double)e[i] * e[i]; }
    const double mean = s / (4096 * 256), sd = sqrt(s2 / (4096 * 256) - mean * mean);
    CHECK(fin && sd > 0.05 && sd < 10, "encoder output finite, mean %.4f sd %.4f", mean, sd);
    /* 两个点(图中心与左下 1/4 处) ⇒ 官方规矩走单掩膜槽 0; 一个点时槽 0 不用, 拿它量交替没意义。外套图上这两点都在布料上(中心上方 1/8 是衣架挂钩, 点上去掩膜就空, 实撞) */
    const float pts[4] = { (float)(w / 2) * tw / w, (float)(h / 2) * th / h, (float)(w / 4) * tw / w, (float)(h * 5 / 8) * th / h }; const int lab[2] = { 1, 1 };
    float iou[4]; seg_buf *low;
    seg_decode(m, emb, pts, lab, 2, NULL, 0, &low, iou);
    float *lo = malloc(SEG_LOWRES * SEG_LOWRES * 4); seg_buf_read(low, 0, lo, SEG_LOWRES * SEG_LOWRES);
    /* 交错指纹: 子像素错位会把 4 张掩膜按列交错(10-15 实撞 ConvT 偏置铺错列), 那时 |lo[x]−lo[x+4]| 远小于 |lo[x]−lo[x+1]|; 对的掩膜是平滑场, 隔 4 的差 ≥ 隔 1 的差 */
    double d1x = 0, d4x = 0, d1y = 0, d4y = 0, t1 = 0, t2 = 0; const int n = SEG_LOWRES * SEG_LOWRES;
    for (int y = 0; y < SEG_LOWRES - 4; y++) for (int x = 0; x < SEG_LOWRES - 4; x++) { const float v = lo[y * SEG_LOWRES + x]; t1 += v; t2 += (double)v * v;
        d1x += fabs(v - lo[y * SEG_LOWRES + x + 1]); d4x += fabs(v - lo[y * SEG_LOWRES + x + 4]); d1y += fabs(v - lo[(y + 1) * SEG_LOWRES + x]); d4y += fabs(v - lo[(y + 4) * SEG_LOWRES + x]); }
    const double lsd = sqrt(t2 / n - (t1 / n) * (t1 / n));
    printf("  lowres: |d1x| %.3f |d4x| %.3f |d1y| %.3f |d4y| %.3f sd %.2f  iou %.3f %.3f %.3f %.3f\n", d1x / n, d4x / n, d1y / n, d4y / n, lsd, iou[0], iou[1], iou[2], iou[3]);
    CHECK(d4x >= d1x && d4y >= d1y && lsd > 1.0, "smooth low-res logits (no sub-pixel interleave), sd %.2f", lsd);
    CHECK(iou[0] > 0.5, "single-mask iou > 0.5 with two points");
    { /* 点必须起作用: 换成左上角背景一个点, IoU 与掩膜占比都该变 */
      size_t in1 = 0; for (int i = 0; i < SEG_LOWRES * SEG_LOWRES; i++) in1 += lo[i] > 0;
      const float bg[2] = { 8.0f, 8.0f }; const int lb[1] = { 1 }; float iou2[4]; seg_buf *low2;
      seg_decode(m, emb, bg, lb, 1, NULL, 0, &low2, iou2); seg_buf_read(low2, (size_t)1 * SEG_LOWRES * SEG_LOWRES, lo, SEG_LOWRES * SEG_LOWRES);
      size_t in2 = 0; for (int i = 0; i < SEG_LOWRES * SEG_LOWRES; i++) in2 += lo[i] > 0;
      printf("  two-point mask: inside %.3f iou %.3f; corner point: inside %.3f iou %.3f %.3f %.3f %.3f\n", (double)in1 / (SEG_LOWRES * SEG_LOWRES), iou[0], (double)in2 / (SEG_LOWRES * SEG_LOWRES), iou2[0], iou2[1], iou2[2], iou2[3]);
      CHECK(fabs(iou2[0] - iou[0]) > 1e-3 && in1 != in2, "prompts change the output"); }
    free(lo); free(e); free(img); free(rs); img_free(&img0); seg_buf_free(dimg); seg_model_free(m); seg_pt_close(pt);
}

int main(int argc, char **argv) {
    const char *pt = argc > 1 ? argv[1] : "gguf/seg/mobile_sam.pt";
    FILE *f = fopen(pt, "rb");
    if (f) { fclose(f); test_pt(pt); } else printf("SKIP: no %s\n", pt);
    char err[256] = "";
    if (seg_dev_init(err, sizeof err)) { seg_ops_tests(); passes += seg_ops_passes; fails += seg_ops_fails; if (f && argc > 2) test_model(pt, argv[2]); seg_dev_free(); }
    else printf("SKIP ops: %s\n", err);
    printf("seg: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
