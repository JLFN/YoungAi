/* img_codec.c — 解码(stb_image: jpg/png/bmp/gif)与 PNG 编码(IHDR/IDAT/IEND, zlib 壳 = 78 01 + office_deflate + adler32)。
 * 只出 PNG: 无损, 不叠一次 JPEG 压缩; 没有 JPEG 编码器也不为一个格式再 vendored 一个库(方案稿 §4)。
 * stb_image 的实现点全仓只有这一处(ds4-seg 与 ds4-train 都链这个 .o); 引擎自己的 core_v41_image.c 另有一份, 两者不同链接。 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#include "../../stb_image.h"
#include "ds4_image.h"
#include "../office/ds4_office.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMG_MAX_PIXELS (64u << 20)   /* 6400 万像素(约 8000×8000): 再大是误传, 四通道 float 中间量就要 1 GB */

bool img_load(const char *path, ds4_img *im, char *err, size_t errn) {
    int w, h, comp; uint8_t *p = stbi_load(path, &w, &h, &comp, 3);
    if (!p) { snprintf(err, errn, "cannot decode %s: %s", path, stbi_failure_reason()); return false; }
    if ((size_t)w * h > IMG_MAX_PIXELS) { stbi_image_free(p); snprintf(err, errn, "%s is %dx%d, over the %u megapixel limit", path, w, h, IMG_MAX_PIXELS >> 20); return false; }
    im->w = w; im->h = h; im->rgb = p;
    return true;
}
bool img_load_rgba(const char *path, int *w, int *h, uint8_t **rgba, char *err, size_t errn) {
    int comp; *rgba = stbi_load(path, w, h, &comp, 4);
    if (!*rgba) { snprintf(err, errn, "cannot decode %s: %s", path, stbi_failure_reason()); return false; }
    if ((size_t)*w * *h > IMG_MAX_PIXELS) { stbi_image_free(*rgba); *rgba = NULL; snprintf(err, errn, "%s too large", path); return false; }
    return true;
}
void img_free(ds4_img *im) { if (im && im->rgb) { stbi_image_free(im->rgb); im->rgb = NULL; } }   /* stb 用 malloc, free 也行; 走它的接口以防 STBI_MALLOC 改了 */

static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void chunk(uint8_t **out, size_t *n, const char *type, const uint8_t *data, size_t len) {
    *out = realloc(*out, *n + len + 12); uint8_t *p = *out + *n;
    be32(p, (uint32_t)len); memcpy(p + 4, type, 4); if (len) memcpy(p + 8, data, len);
    be32(p + 8 + len, office_crc32(p + 4, len + 4)); *n += len + 12;
}
uint8_t *img_png_encode(int w, int h, int comps, const uint8_t *px, size_t *n) {
    const size_t rown = (size_t)w * comps + 1; uint8_t *raw = malloc(rown * (size_t)h);
    for (int y = 0; y < h; y++) { raw[(size_t)y * rown] = 0; memcpy(raw + (size_t)y * rown + 1, px + (size_t)y * w * comps, (size_t)w * comps); }   /* 滤波 0: 简单, deflate 的 LZ 足以压平坦区 */
    uint32_t a = 1, b = 0; for (size_t i = 0; i < rown * (size_t)h; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    uint8_t *df = NULL; const size_t dn = office_deflate(raw, rown * (size_t)h, &df); free(raw);
    uint8_t *z = malloc(dn + 6); z[0] = 0x78; z[1] = 0x01; memcpy(z + 2, df, dn); be32(z + 2 + dn, (b << 16) | a); free(df);
    uint8_t *out = NULL; *n = 0;
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 }; out = malloc(8); memcpy(out, sig, 8); *n = 8;
    uint8_t ih[13]; be32(ih, (uint32_t)w); be32(ih + 4, (uint32_t)h); ih[8] = 8; ih[9] = comps == 1 ? 0 : comps == 3 ? 2 : 6; ih[10] = ih[11] = ih[12] = 0;
    chunk(&out, n, "IHDR", ih, 13); chunk(&out, n, "IDAT", z, dn + 6); chunk(&out, n, "IEND", NULL, 0);
    free(z); return out;
}
bool img_png_write(const char *path, int w, int h, int comps, const uint8_t *px) {
    size_t n; uint8_t *bytes = img_png_encode(w, h, comps, px, &n);
    FILE *f = fopen(path, "wb"); if (!f) { free(bytes); return false; }
    const bool ok = fwrite(bytes, 1, n, f) == n; free(bytes);
    return fclose(f) == 0 && ok;
}

uint32_t img_parse_color(const char *s, bool *ok) {
    static const struct { const char *n; uint32_t v; } NAMES[] = {
        { "black", 0x000000 }, { "white", 0xffffff }, { "red", 0xff0000 }, { "green", 0x008000 }, { "blue", 0x0000ff }, { "yellow", 0xffff00 }, { "orange", 0xffa500 },
        { "purple", 0x800080 }, { "pink", 0xffc0cb }, { "gray", 0x808080 }, { "grey", 0x808080 }, { "brown", 0x8b4513 }, { "navy", 0x000080 }, { "beige", 0xf5f5dc }, { "cyan", 0x00ffff },
    };
    *ok = true;
    if (!s) { *ok = false; return 0; }
    if (s[0] == '#') {
        const size_t n = strlen(s + 1); unsigned v = 0;
        if ((n != 6 && n != 3) || sscanf(s + 1, "%x", &v) != 1) { *ok = false; return 0; }
        if (n == 3) v = ((v & 0xf00) * 0x1100) | ((v & 0x0f0) * 0x110) | ((v & 0x00f) * 0x11);
        return v;
    }
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) if (!strcasecmp(s, NAMES[i].n)) return NAMES[i].v;
    *ok = false; return 0;
}
