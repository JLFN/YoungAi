/* office_img.c — 图片头识别: docx/pptx 嵌图只要知道格式与像素尺寸(算排版宽高), 像素原样塞进包里, 不解码。
 * PNG: 8 字节签名后第一个块必是 IHDR, 宽高是 16/20 偏移的大端 32 位。
 * JPEG: 顺着段走到 SOF0/1/2(基线/扩展/渐进), 段内 1 字节精度后面是高、宽(大端 16 位)。 */
#include "ds4_office.h"
#include <stdlib.h>
#include <string.h>

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }

bool office_img_info(const uint8_t *p, size_t n, office_imginfo *i) {
    static const uint8_t PNG[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (n >= 24 && !memcmp(p, PNG, 8) && !memcmp(p + 12, "IHDR", 4)) {
        i->w = (int)be32(p + 16); i->h = (int)be32(p + 20); i->ext = "png"; i->mime = "image/png";
        const int ct = p[25]; i->comps = ct == 0 ? 1 : ct == 2 ? 3 : ct == 3 ? 3 : 4;
        return i->w > 0 && i->h > 0;
    }
    if (n >= 4 && p[0] == 0xFF && p[1] == 0xD8) {
        size_t k = 2;
        while (k + 9 <= n && p[k] == 0xFF) {
            const uint8_t m = p[k + 1];
            if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01 || m == 0xFF) { k += m == 0xFF ? 1 : 2; continue; }   /* 无长度的段 / 填充 */
            const size_t seg = be16(p + k + 2);
            if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
                i->h = (int)be16(p + k + 5); i->w = (int)be16(p + k + 7); i->comps = p[k + 9]; i->ext = "jpeg"; i->mime = "image/jpeg";
                return i->w > 0 && i->h > 0;
            }
            if (m == 0xDA || m == 0xD9) break;   /* 到扫描数据/结尾还没见 SOF: 不是我们认得的 JPEG */
            k += 2 + seg;
        }
    }
    return false;
}

/* ---------------- PNG 解码: IDAT 拼起来 inflate, 逐行去滤波(None/Sub/Up/Average/Paeth), 展成 RGBA ---------------- */
static uint8_t paeth(int a, int b, int c) { const int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c); return (uint8_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c); }

bool office_png_decode(const uint8_t *p, size_t n, int *w, int *h, uint8_t **rgba) {
    office_imginfo info;
    if (!office_img_info(p, n, &info) || strcmp(info.ext, "png")) return false;
    const int depth = p[24], ctype = p[25], interlace = p[28];
    if (depth != 8 || interlace) return false;
    const int ch = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
    if (!ch) return false;
    ds4_buf idat = {0}; const uint8_t *plte = NULL; size_t plte_n = 0;
    for (size_t k = 8; k + 12 <= n; ) {
        const uint32_t len = be32(p + k); const uint8_t *type = p + k + 4;
        if (k + 12 + len > n) break;
        if (!memcmp(type, "IDAT", 4)) ds4_buf_append(&idat, p + k + 8, len);
        else if (!memcmp(type, "PLTE", 4)) { plte = p + k + 8; plte_n = len / 3; }
        else if (!memcmp(type, "IEND", 4)) break;
        k += 12 + len;
    }
    if (idat.len < 2) { ds4_buf_free(&idat); return false; }
    uint8_t *raw; size_t rn;
    const bool ok = office_inflate((const uint8_t *)idat.ptr + 2, idat.len - 2, &raw, &rn);   /* 跳过 2 字节 zlib 头; 尾部 adler 当作多余字节, inflate 到 final 块就停 */
    ds4_buf_free(&idat);
    if (!ok) return false;
    const int W = info.w, H = info.h; const size_t stride = (size_t)W * (size_t)ch;
    if (rn < (stride + 1) * (size_t)H) { free(raw); return false; }
    uint8_t *out = malloc((size_t)W * (size_t)H * 4), *prev = calloc(stride, 1), *cur = malloc(stride);
    for (int y = 0; y < H; y++) {
        const uint8_t *line = raw + (size_t)y * (stride + 1); const int ft = line[0]; line++;
        for (size_t i = 0; i < stride; i++) {
            const int a = i >= (size_t)ch ? cur[i - (size_t)ch] : 0, b = prev[i], c = i >= (size_t)ch ? prev[i - (size_t)ch] : 0;
            uint8_t v = line[i];
            if (ft == 1) v = (uint8_t)(v + a); else if (ft == 2) v = (uint8_t)(v + b); else if (ft == 3) v = (uint8_t)(v + (a + b) / 2); else if (ft == 4) v = (uint8_t)(v + paeth(a, b, c));
            cur[i] = v;
        }
        for (int x = 0; x < W; x++) {
            uint8_t *o = out + ((size_t)y * (size_t)W + (size_t)x) * 4; const uint8_t *s = cur + (size_t)x * (size_t)ch;
            if (ctype == 3) { const size_t idx = s[0] < plte_n ? s[0] : 0; o[0] = plte ? plte[idx * 3] : 0; o[1] = plte ? plte[idx * 3 + 1] : 0; o[2] = plte ? plte[idx * 3 + 2] : 0; o[3] = 255; }
            else if (ch == 1) { o[0] = o[1] = o[2] = s[0]; o[3] = 255; }
            else if (ch == 2) { o[0] = o[1] = o[2] = s[0]; o[3] = s[1]; }
            else { o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = ch == 4 ? s[3] : 255; }
        }
        uint8_t *t = prev; prev = cur; cur = t;
    }
    free(raw); free(prev); free(cur);
    *w = W; *h = H; *rgba = out;
    return true;
}
