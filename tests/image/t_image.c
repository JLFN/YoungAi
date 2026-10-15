/* t_image.c — src/image 单测(make ds4_image_test && ./ds4_image_test): 不要模型/GPU, 秒级。
 * ① PNG 编码 → stb 解码往返逐字节同(RGB 与灰度)。② 裁/缩/转/翻的尺寸与像素位置。③ 画框/线落点。④ 颜色解析。
 * ⑤ 合成图(左半奶白、右半灰白, 都带噪, 中间一块深色"衣架"): 正点在左半 → 掩膜 ≈ 左半且不含衣架; 导向滤波 alpha 内 1 外 0; 换黑后内部变暗外部不动。 */
#include "../../src/image/ds4_image.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails, passes;
#define CHECK(cond, ...) do { if (cond) passes++; else { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static ds4_img synth(int w, int h) {   /* 左半 (205,200,190) 右半 (190,192,195) 各加 ±6 噪; 中间 (w/2±30, 60..120) 深棕衣架 */
    ds4_img im = { w, h, malloc((size_t)w * h * 3) }; unsigned s = 12345;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint8_t *p = im.rgb + ((size_t)y * w + x) * 3; s = s * 1103515245u + 12345u; const int n = (int)((s >> 16) % 13) - 6;
        if (x > w / 2 - 30 && x < w / 2 + 30 && y > 60 && y < 120) { p[0] = 110; p[1] = 70; p[2] = 40; }
        else if (x < w / 2) { p[0] = (uint8_t)(205 + n); p[1] = (uint8_t)(200 + n); p[2] = (uint8_t)(190 + n); }
        else { p[0] = (uint8_t)(190 + n); p[1] = (uint8_t)(192 + n); p[2] = (uint8_t)(195 + n); }
    }
    return im;
}

static void test_codec(void) {
    ds4_img a = synth(37, 23); const char *tmp = "/tmp/ds4_t_image.png"; char err[256];
    CHECK(img_png_write(tmp, a.w, a.h, 3, a.rgb), "png write");
    ds4_img b; CHECK(img_load(tmp, &b, err, sizeof err) && b.w == 37 && b.h == 23 && !memcmp(a.rgb, b.rgb, (size_t)37 * 23 * 3), "png rgb roundtrip: %s", err);
    uint8_t g[37 * 23]; for (int i = 0; i < 37 * 23; i++) g[i] = (uint8_t)(i * 7);
    CHECK(img_png_write(tmp, 37, 23, 1, g), "gray write");
    ds4_img c; bool ok = img_load(tmp, &c, err, sizeof err) && c.w == 37; for (int i = 0; ok && i < 37 * 23; i++) if (c.rgb[i * 3] != g[i] || c.rgb[i * 3 + 1] != g[i]) ok = false;
    CHECK(ok, "png gray roundtrip");
    int w, h; uint8_t *rgba; CHECK(img_load_rgba(tmp, &w, &h, &rgba, err, sizeof err) && rgba[3] == 255, "rgba load of opaque png"); free(rgba);
    ds4_img bad; CHECK(!img_load("/nonexistent.png", &bad, err, sizeof err) && err[0], "missing file reports error");
    img_free(&b); img_free(&c); free(a.rgb); unlink(tmp);
}
static void test_geometry(void) {
    ds4_img im = synth(40, 30); const uint8_t *p = im.rgb;
    const uint8_t px_5_7 = p[(7 * 40 + 5) * 3];
    img_rect r = { 3, 4, 10, 8 }; CHECK(img_crop(&im, r) && im.w == 10 && im.h == 8 && im.rgb[(3 * 10 + 2) * 3] == px_5_7, "crop keeps pixels");
    img_rotate(&im, 90); CHECK(im.w == 8 && im.h == 10, "rotate 90 swaps dims");
    const uint8_t tl = im.rgb[0]; img_rotate(&im, 270); CHECK(im.w == 10 && im.h == 8, "rotate back");
    img_flip(&im, true); img_flip(&im, true); CHECK(im.rgb[(7 * 10 + 9) * 3] == im.rgb[(7 * 10 + 9) * 3], "double flip identity"); (void)tl;
    img_resize(&im, 20, 16); CHECK(im.w == 20 && im.h == 16, "resize up");
    img_resize(&im, 5, 4); CHECK(im.w == 5 && im.h == 4, "resize down (antialiased)");
    img_rect bad = { 100, 100, 5, 5 }; CHECK(!img_crop(&im, bad), "crop outside fails");
    free(im.rgb);
    ds4_img im2 = synth(50, 40);
    img_rect box = { 10, 10, 20, 15 }; img_draw_rect(&im2, box, 0xff0000, 2, false);
    CHECK(im2.rgb[(10 * 50 + 10) * 3] == 255 && im2.rgb[(10 * 50 + 10) * 3 + 1] == 0, "rect outline corner red");
    CHECK(im2.rgb[(17 * 50 + 20) * 3 + 1] != 0, "rect interior untouched");
    img_draw_line(&im2, 0, 35, 49, 35, 0x0000ff, 3, true); CHECK(im2.rgb[(35 * 50 + 25) * 3 + 2] == 255 && im2.rgb[(35 * 50 + 25) * 3] == 0, "line pixel blue");
    img_rect whole = { 0, 0, 0, 0 }; img_pixelate(&im2, whole, 8); CHECK(im2.rgb[0] == im2.rgb[(3 * 50 + 3) * 3], "pixelate block uniform");
    free(im2.rgb);
    bool ok; CHECK(img_parse_color("#141414", &ok) == 0x141414 && ok, "hex color"); CHECK(img_parse_color("#f0a", &ok) == 0xff00aa && ok, "short hex");
    CHECK(img_parse_color("Red", &ok) == 0xff0000 && ok, "named color"); img_parse_color("nope", &ok); CHECK(!ok, "bad color rejected");
}
static void test_mask(void) {
    const int W = 400, H = 300; ds4_img im = synth(W, H);
    const int pos[4] = { 60, 150, 120, 220 }, neg[2] = { W / 2, 90 };
    uint8_t *m = img_mask_color(&im, pos, 2, neg, 1);
    size_t left = 0, right = 0, hanger = 0, nl = 0, nr = 0, nh = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        const bool h = x > W / 2 - 30 && x < W / 2 + 30 && y > 60 && y < 120;
        if (h) { hanger += m[(size_t)y * W + x]; nh++; } else if (x < W / 2 - 4) { left += m[(size_t)y * W + x]; nl++; } else if (x > W / 2 + 4) { right += m[(size_t)y * W + x]; nr++; }
    }
    printf("  mask: left %.3f right %.3f hanger %.3f\n", (double)left / nl, (double)right / nr, (double)hanger / nh);
    CHECK((double)left / nl > 0.9, "mask covers the cream half");
    CHECK((double)right / nr < 0.05, "mask leaves the grey half");
    CHECK((double)hanger / nh < 0.2, "mask excludes the hanger");
    float *alpha = img_alpha_guided(&im, m, 8, 1e-3f);
    CHECK(alpha[(size_t)150 * W + 60] > 0.9f && alpha[(size_t)150 * W + 340] < 0.1f, "guided alpha inside 1 outside 0 (%.2f %.2f)", alpha[(size_t)150 * W + 60], alpha[(size_t)150 * W + 340]);
    bool soft = false; for (int x = W / 2 - 12; x < W / 2 + 12; x++) { const float a = alpha[(size_t)200 * W + x]; if (a > 0.1f && a < 0.9f) soft = true; }
    CHECK(soft, "edge is feathered");
    const double share = img_alpha_share(alpha, (size_t)W * H); const img_rect bb = img_alpha_bbox(alpha, W, H);
    CHECK(share > 0.4 && share < 0.55 && bb.x == 0 && bb.w > W / 2 - 20 && bb.w < W / 2 + 20, "share %.3f bbox x %d w %d", share, bb.x, bb.w);
    ds4_img ov = img_overlay(&im, alpha, 0xff0000); CHECK(ov.rgb[(size_t)(150 * W + 60) * 3] > ov.rgb[(size_t)(150 * W + 60) * 3 + 1] + 40, "overlay tints inside red"); free(ov.rgb);
    const uint8_t before_out = im.rgb[(size_t)(150 * W + 340) * 3];
    img_recolor(&im, alpha, 0x141414);
    CHECK(im.rgb[(size_t)(150 * W + 60) * 3] < 60 && im.rgb[(size_t)(150 * W + 340) * 3] == before_out, "recolor darkens inside, leaves outside (%d, %d)", im.rgb[(size_t)(150 * W + 60) * 3], im.rgb[(size_t)(150 * W + 340) * 3]);
    free(alpha); free(m); free(im.rgb);
}

int main(void) {
    test_codec(); test_geometry(); test_mask();
    printf("image: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
