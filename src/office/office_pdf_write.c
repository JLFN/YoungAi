/* office_pdf_write.c — PDF 的对象层: 对象表 / 流(FlateDecode 用我们自己的 deflate) / 字体对象(内嵌 TrueType 子集或非内嵌 STSong-Light) / 图片对象 / 页树 / xref / trailer。
 * 排版(谁在哪一页的哪个位置)在 office_pdf.c, 这里只管把它交来的内容流和资源写成合法的 PDF 1.4。
 * 字体两条路(方案稿 §3.3):
 *   内嵌: Type0 + CIDFontType2 + FontFile2(子集 TTF) + CIDToGIDMap /Identity + ToUnicode(新字形号 → 码点, 不写它 pdftotext/复制就是乱码); 内容流里的字符串 = 新字形号的 2 字节;
 *   非内嵌: Type0 + CIDFontType0 /STSong-Light /UniGB-UCS2-H(Adobe-GB1), 内容流字符串 = UCS-2BE; 没装中文字体的 Linux 阅读器会显示方块, 结果文本里要说。
 * Courier(标准 14 字体)给代码用, 不嵌。 */
#include "office_internal.h"
#include "office_pdf.h"
#include <stdio.h>

int pdf_reserve(pdfdoc *d) {
    if (d->nobj == d->cap) { d->cap = d->cap ? d->cap * 2 : 64; d->objs = realloc(d->objs, sizeof *d->objs * (size_t)d->cap); }
    d->objs[d->nobj] = (ds4_buf){0};
    return ++d->nobj;   /* 对象号从 1 起 */
}
void pdf_set(pdfdoc *d, int num, const char *body, size_t n) { ds4_buf *b = &d->objs[num - 1]; b->len = 0; ds4_buf_append(b, body, n); }
int pdf_obj(pdfdoc *d, const char *body) { const int n = pdf_reserve(d); pdf_set(d, n, body, strlen(body)); return n; }
/* 流对象: compress 时走 deflate(zlib 包装: 2 字节头 + adler32), 压不小就原样 */
static uint32_t adler32(const uint8_t *p, size_t n) { uint32_t a = 1, b = 0; for (size_t i = 0; i < n; i++) { a = (a + p[i]) % 65521; b = (b + a) % 65521; } return b << 16 | a; }
void pdf_set_stream(pdfdoc *d, int num, const char *dict_extra, const uint8_t *data, size_t n, bool compress) {
    ds4_buf *b = &d->objs[num - 1]; b->len = 0;
    uint8_t *def = NULL; size_t dn = 0;
    if (compress && n) { dn = office_deflate(data, n, &def); if (dn + 6 >= n) { free(def); def = NULL; } }
    ds4_buf_printf(b, "<< /Length %zu %s %s >>\nstream\n", def ? dn + 6 : n, def ? "/Filter /FlateDecode" : "", dict_extra ? dict_extra : "");
    if (def) { const uint32_t ad = adler32(data, n); ds4_buf_putc(b, 0x78); ds4_buf_putc(b, 0x01); ds4_buf_append(b, def, dn); char t[4] = { (char)(ad >> 24), (char)(ad >> 16), (char)(ad >> 8), (char)ad }; ds4_buf_append(b, t, 4); free(def); }
    else ds4_buf_append(b, data, n);
    ds4_buf_puts(b, "\nendstream");
}
int pdf_stream(pdfdoc *d, const char *dict_extra, const uint8_t *data, size_t n, bool compress) { const int num = pdf_reserve(d); pdf_set_stream(d, num, dict_extra, data, n, compress); return num; }

/* PDF 字符串字面量: ( ) \ 转义, 非 ASCII 走 UTF-16BE 带 BOM(标题里有中文) */
void pdf_text_string(ds4_buf *b, const char *s) {
    bool ascii = true; for (const char *p = s; *p; p++) if ((unsigned char)*p >= 0x80 || (unsigned char)*p < 0x20) { ascii = false; break; }
    if (ascii) { ds4_buf_putc(b, '('); for (; *s; s++) { if (*s == '(' || *s == ')' || *s == '\\') ds4_buf_putc(b, '\\'); ds4_buf_putc(b, *s); } ds4_buf_putc(b, ')'); return; }
    ds4_buf_puts(b, "<FEFF");
    for (const char *p = s; *p; ) { uint32_t cp; const int k = office_utf8_next(p, &cp); p += k; if (cp > 0xFFFF) { cp -= 0x10000; ds4_buf_printf(b, "%04X%04X", 0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF)); } else ds4_buf_printf(b, "%04X", cp); }
    ds4_buf_putc(b, '>');
}

int office_utf8_next(const char *s, uint32_t *cp) {
    const unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) { *cp = (uint32_t)(c & 0x1F) << 6 | (s[1] & 0x3F); return 2; }
    if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) { *cp = (uint32_t)(c & 0x0F) << 12 | (uint32_t)(s[1] & 0x3F) << 6 | (s[2] & 0x3F); return 3; }
    if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) { *cp = (uint32_t)(c & 0x07) << 18 | (uint32_t)(s[1] & 0x3F) << 12 | (uint32_t)(s[2] & 0x3F) << 6 | (s[3] & 0x3F); return 4; }
    *cp = 0xFFFD; return 1;
}

/* ---------------- 字体对象 ---------------- */
/* 内嵌: 子集化 → FontFile2; W 数组按新字形号给步进; ToUnicode 按新字形号给码点 */
static int font_embedded(pdfdoc *d, pdffont *f) {
    int n = f->nnew; size_t len; uint8_t *ttf = office_ttf_subset(f->ttf, f->new_of_old, &n, &len);
    char extra[64]; snprintf(extra, sizeof extra, "/Length1 %zu", len);
    const int file = pdf_stream(d, extra, ttf, len, true); free(ttf);
    const int *bb = office_ttf_bbox(f->ttf);
    ds4_buf b = {0};
    ds4_buf_printf(&b, "<< /Type /FontDescriptor /FontName /DWARF+%s /Flags 4 /FontBBox [%d %d %d %d] /ItalicAngle 0 /Ascent %d /Descent %d /CapHeight %d /StemV 80 /FontFile2 %d 0 R >>",
                   office_ttf_name(f->ttf), bb[0], bb[1], bb[2], bb[3], office_ttf_ascent(f->ttf), office_ttf_descent(f->ttf), office_ttf_ascent(f->ttf) * 7 / 10, file);
    const int desc = pdf_obj(d, b.ptr); b.len = 0;
    ds4_buf_puts(&b, "[");
    for (int g = 0; g < office_ttf_nglyphs(f->ttf); g++) if (f->new_of_old[g] >= 0 && f->new_of_old[g] < f->nnew) ds4_buf_printf(&b, " %d [%d]", f->new_of_old[g], office_ttf_advance(f->ttf, g));
    ds4_buf_puts(&b, " ]");
    ds4_buf w = {0};
    ds4_buf_printf(&w, "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /DWARF+%s /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> /FontDescriptor %d 0 R /DW 1000 /W %s /CIDToGIDMap /Identity >>",
                   office_ttf_name(f->ttf), desc, b.ptr);
    const int cid = pdf_obj(d, w.ptr); b.len = 0; w.len = 0;
    /* ToUnicode: 一个 bfchar 一行; 码点 > FFFF 写代理对 */
    ds4_buf_puts(&b, "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n");
    int cnt = 0; for (int i = 1; i < f->nnew; i++) if (f->cp_of_new[i]) cnt++;
    for (int i = 1, k = 0; i < f->nnew; i++) {
        if (!f->cp_of_new[i]) continue;
        if (k % 100 == 0) ds4_buf_printf(&b, "%d beginbfchar\n", cnt - k < 100 ? cnt - k : 100);
        uint32_t cp = f->cp_of_new[i];
        if (cp > 0xFFFF) { cp -= 0x10000; ds4_buf_printf(&b, "<%04X> <%04X%04X>\n", i, 0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF)); } else ds4_buf_printf(&b, "<%04X> <%04X>\n", i, cp);
        if (++k % 100 == 0 || k == cnt) ds4_buf_puts(&b, "endbfchar\n");
    }
    ds4_buf_puts(&b, "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n");
    const int tu = pdf_stream(d, NULL, (const uint8_t *)b.ptr, b.len, true);
    ds4_buf_printf(&w, "<< /Type /Font /Subtype /Type0 /BaseFont /DWARF+%s /Encoding /Identity-H /DescendantFonts [%d 0 R] /ToUnicode %d 0 R >>", office_ttf_name(f->ttf), cid, tu);
    const int font = pdf_obj(d, w.ptr);
    ds4_buf_free(&b); ds4_buf_free(&w);
    return font;
}
/* 非内嵌: Adobe-GB1 的 STSong-Light; ASCII 的比例宽度照 Helvetica 给(阅读器替换的字体也是比例字体, 不给就按 1000 排, 英文全是大空格) */
static const uint8_t HELV_W[95] = { 28, 28, 36, 56, 56, 89, 67, 19, 33, 33, 39, 58, 28, 33, 28, 28, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 28, 28, 58, 58, 58, 56, 102, 67, 67, 72, 72, 67, 61, 78, 72, 28, 50, 67, 56, 83, 72, 78, 67, 78, 72, 67, 61, 72, 67, 94, 67, 67, 61, 28, 28, 28, 47, 56, 33, 56, 56, 50, 56, 56, 28, 56, 56, 22, 22, 50, 22, 83, 56, 56, 56, 56, 33, 50, 28, 56, 50, 72, 50, 50, 50, 33, 26, 33, 58 };
int pdf_helv_width(uint32_t cp) { return cp >= 32 && cp < 127 ? HELV_W[cp - 32] * 10 : 1000; }
static int font_cjk_builtin(pdfdoc *d) {
    ds4_buf b = {0};
    ds4_buf_puts(&b, "<< /Type /FontDescriptor /FontName /STSong-Light /Flags 4 /FontBBox [-25 -254 1000 880] /ItalicAngle 0 /Ascent 880 /Descent -120 /CapHeight 700 /StemV 80 >>");
    const int desc = pdf_obj(d, b.ptr); b.len = 0;
    ds4_buf_printf(&b, "<< /Type /Font /Subtype /CIDFontType0 /BaseFont /STSong-Light /CIDSystemInfo << /Registry (Adobe) /Ordering (GB1) /Supplement 5 >> /FontDescriptor %d 0 R /DW 1000 /W [1 [", desc);
    for (int i = 0; i < 95; i++) ds4_buf_printf(&b, "%d ", HELV_W[i] * 10);   /* GB1 的 CID 1..95 = ASCII 32..126 的比例字形 */
    ds4_buf_puts(&b, "]] >>");
    const int cid = pdf_obj(d, b.ptr); b.len = 0;
    ds4_buf_printf(&b, "<< /Type /Font /Subtype /Type0 /BaseFont /STSong-Light /Encoding /UniGB-UCS2-H /DescendantFonts [%d 0 R] >>", cid);
    const int font = pdf_obj(d, b.ptr); ds4_buf_free(&b);
    return font;
}
int pdf_font_objects(pdfdoc *d, pdffont *f) { return f->ttf ? font_embedded(d, f) : font_cjk_builtin(d); }
int pdf_courier(pdfdoc *d) { return pdf_obj(d, "<< /Type /Font /Subtype /Type1 /BaseFont /Courier /Encoding /WinAnsiEncoding >>"); }

/* ---------------- 图片对象: JPEG 原样(DCTDecode), PNG 解码成 RGB + 可选 SMask(alpha) ---------------- */
int pdf_image(pdfdoc *d, const uint8_t *data, size_t n, const office_imginfo *info) {
    char extra[256];
    if (!strcmp(info->ext, "jpeg")) {
        const char *cs = info->comps == 1 ? "/DeviceGray" : info->comps == 4 ? "/DeviceCMYK /Decode [1 0 1 0 1 0 1 0]" : "/DeviceRGB";   /* Adobe 的 CMYK JPEG 是反的 */
        snprintf(extra, sizeof extra, "/Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace %s /BitsPerComponent 8 /Filter /DCTDecode", info->w, info->h, cs);
        return pdf_stream(d, extra, data, n, false);
    }
    int w, h; uint8_t *rgba;
    if (!office_png_decode(data, n, &w, &h, &rgba)) return 0;
    const size_t px = (size_t)w * (size_t)h; uint8_t *rgb = malloc(px * 3), *a = malloc(px); bool alpha = false;
    for (size_t i = 0; i < px; i++) { rgb[i * 3] = rgba[i * 4]; rgb[i * 3 + 1] = rgba[i * 4 + 1]; rgb[i * 3 + 2] = rgba[i * 4 + 2]; a[i] = rgba[i * 4 + 3]; if (a[i] != 255) alpha = true; }
    free(rgba);
    int smask = 0;
    if (alpha) { snprintf(extra, sizeof extra, "/Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceGray /BitsPerComponent 8", w, h); smask = pdf_stream(d, extra, a, px, true); }
    if (smask) snprintf(extra, sizeof extra, "/Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceRGB /BitsPerComponent 8 /SMask %d 0 R", w, h, smask);
    else snprintf(extra, sizeof extra, "/Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceRGB /BitsPerComponent 8", w, h);
    const int img = pdf_stream(d, extra, rgb, px * 3, true);
    free(rgb); free(a);
    return img;
}

/* ---------------- 收尾: 页对象、页树、目录、信息、xref ---------------- */
uint8_t *pdf_finish(pdfdoc *d, pdfpage *pages, int npages, const char *resources, const char *title, time_t now, size_t *len) {
    const int tree = pdf_reserve(d);
    ds4_buf kids = {0};
    for (int i = 0; i < npages; i++) {
        const int content = pdf_stream(d, NULL, (const uint8_t *)pages[i].content.ptr, pages[i].content.len, true);
        ds4_buf p = {0};
        ds4_buf_printf(&p, "<< /Type /Page /Parent %d 0 R /MediaBox [0 0 %.2f %.2f] /Resources %s /Contents %d 0 R", tree, PDF_PAGE_W, PDF_PAGE_H, resources, content);
        if (pages[i].annots.len) { ds4_buf_puts(&p, " /Annots ["); ds4_buf_append(&p, pages[i].annots.ptr, pages[i].annots.len); ds4_buf_puts(&p, "]"); }
        ds4_buf_puts(&p, " >>");
        ds4_buf_printf(&kids, "%d 0 R ", pdf_obj(d, p.ptr)); ds4_buf_free(&p);
    }
    ds4_buf t = {0}; ds4_buf_printf(&t, "<< /Type /Pages /Kids [%s] /Count %d >>", kids.ptr ? kids.ptr : "", npages); pdf_set(d, tree, t.ptr, t.len); t.len = 0; ds4_buf_free(&kids);
    ds4_buf_printf(&t, "<< /Type /Catalog /Pages %d 0 R >>", tree); const int root = pdf_obj(d, t.ptr); t.len = 0;
    char ts[32]; struct tm tm; gmtime_r(&now, &tm); strftime(ts, sizeof ts, "%Y%m%d%H%M%S", &tm);
    ds4_buf_puts(&t, "<< /Producer (DwarfStar) /Title "); pdf_text_string(&t, title ? title : ""); ds4_buf_printf(&t, " /CreationDate (D:%sZ) /ModDate (D:%sZ) >>", ts, ts);
    const int info = pdf_obj(d, t.ptr); ds4_buf_free(&t);
    ds4_buf out = {0}; ds4_buf_puts(&out, "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n");
    size_t *off = malloc(sizeof *off * (size_t)(d->nobj + 1));
    for (int i = 0; i < d->nobj; i++) { off[i + 1] = out.len; ds4_buf_printf(&out, "%d 0 obj\n", i + 1); ds4_buf_append(&out, d->objs[i].ptr, d->objs[i].len); ds4_buf_puts(&out, "\nendobj\n"); }
    const size_t xref = out.len;
    ds4_buf_printf(&out, "xref\n0 %d\n0000000000 65535 f \n", d->nobj + 1);
    for (int i = 1; i <= d->nobj; i++) ds4_buf_printf(&out, "%010zu 00000 n \n", off[i]);
    ds4_buf_printf(&out, "trailer\n<< /Size %d /Root %d 0 R /Info %d 0 R >>\nstartxref\n%zu\n%%%%EOF\n", d->nobj + 1, root, info, xref);
    free(off);
    for (int i = 0; i < d->nobj; i++) ds4_buf_free(&d->objs[i]);
    free(d->objs); memset(d, 0, sizeof *d);
    *len = out.len;
    return (uint8_t *)out.ptr;
}
