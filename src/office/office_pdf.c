/* office_pdf.c — 文档树 → PDF 排版: 自己换行、分页、画表格与图, 把每页的内容流交给 office_pdf_write.c。
 * 版式写死(方案稿 §3.4): A4, 边距 2.54 cm, 正文 11 pt 行距 1.5, 标题 20/16/13/12, 表格 0.5 pt 网格 + 表头浅底, 代码浅底等宽, 页脚居中页码。
 * 换行按字形宽度: 空格后可断, 中日韩每个字后可断, 一个词超一行就按字断。粗体 = 描边加粗(Tr 2), 斜体 = 文本矩阵斜切 —— 只嵌一个字面, 不嵌四个。
 * 字体: 本机有 glyf 中文 TrueType 就嵌子集; 没有退非内嵌 STSong-Light 并记进 report(没装中文字体的 Linux 阅读器显示方块)。代码里的 ASCII 用 Courier(标准 14 字体, 不嵌)。 */
#include "office_pdf.h"
#include "office_internal.h"
#include <math.h>
#include <stdio.h>

#define BODY 11.0
#define LEAD 1.5
#define IND 18.0

typedef struct { uint32_t cp; double w; uint8_t bold, italic, code; int link; } ch;   /* 一个字符: 宽度已按它的字号算好 */
typedef struct {
    pdfdoc d; pdffont font, sym; pdfpage *pages; int npages, cap_pages;   /* sym: 主字体缺的符号走它(/F4), 没有就 ttf=NULL */
    double y;                   /* 当前页的书写位置(基线上方的顶线) */
    const office_opts *o; office_report *rep;
    ds4_buf xobjs;              /* 资源里的 /ImN 条目 */
    int nimg;
    char **links; int nlinks;   /* 链接地址表 */
    bool builtin;               /* 非内嵌路 */
    bool helv_ascii;            /* 主字体没有拉丁字形(spark 的 DroidSansFallback 只有中日韩): ASCII 走标准 14 字体 Helvetica(/F3, 不嵌) */
} pw;
static bool use_helv(const pw *w, uint32_t cp) { return w->helv_ascii && cp < 128; }
enum { F_MAIN = 1, F_COUR, F_HELV, F_SYM };   /* 资源里的 /F1../F4 */
/* 一个字用哪个字体: 代码 ASCII → Courier; 主字体没拉丁 → Helvetica; 主字体有 → 主; 主没有而后备有 → 后备; 都没有 → 主(落 ? 或方块) */
static int font_for(const pw *w, uint32_t cp, bool code) {
    if (code && cp < 128) return F_COUR;
    if (w->builtin) return F_MAIN;
    if (use_helv(w, cp)) return F_HELV;
    if (office_ttf_gid(w->font.ttf, cp) || !w->sym.ttf || !office_ttf_gid(w->sym.ttf, cp)) return F_MAIN;
    return F_SYM;
}

static pdfpage *page(pw *w) { return &w->pages[w->npages - 1]; }
static void new_page(pw *w) {
    if (w->npages == w->cap_pages) { w->cap_pages = w->cap_pages ? w->cap_pages * 2 : 8; w->pages = realloc(w->pages, sizeof *w->pages * (size_t)w->cap_pages); }
    w->pages[w->npages++] = (pdfpage){ {0}, {0} };
    w->y = PDF_TOP;
}
static void need(pw *w, double h) { if (w->y - h < PDF_BOTTOM && w->y < PDF_TOP - 1) new_page(w); }

/* 字形号: 内嵌路按首次使用分配新号 */
static int gid_in(pdffont *f, uint32_t cp) {
    int g = office_ttf_gid(f->ttf, cp); if (!g && cp != 0xFFFD) g = office_ttf_gid(f->ttf, 0xFFFD); if (!g) g = office_ttf_gid(f->ttf, '?');
    if (f->new_of_old[g] < 0) {
        if (f->nnew == f->cap_new) { f->cap_new *= 2; f->cp_of_new = realloc(f->cp_of_new, sizeof(uint32_t) * (size_t)f->cap_new); }
        f->new_of_old[g] = f->nnew; f->cp_of_new[f->nnew] = cp; f->nnew++;
    }
    return g;
}
static int gid_new(pw *w, uint32_t cp) { return gid_in(&w->font, cp); }
static void font_init(pdffont *f) {
    const int ng = office_ttf_nglyphs(f->ttf);
    f->new_of_old = malloc(sizeof(int) * (size_t)ng); for (int i = 0; i < ng; i++) f->new_of_old[i] = -1;
    f->cap_new = 256; f->cp_of_new = calloc((size_t)f->cap_new, sizeof(uint32_t)); f->new_of_old[0] = 0; f->nnew = 1;
}
static bool is_cjk(uint32_t cp) { return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x2FFFF) || cp == 0x3000; }
static double cw(pw *w, uint32_t cp, double size, bool code) {   /* 字符宽(pt) */
    const int f = font_for(w, cp, code);
    if (f == F_COUR) return 600.0 * size / 1000;
    if (w->builtin || f == F_HELV) return (is_cjk(cp) ? 1000 : pdf_helv_width(cp)) * size / 1000;
    if (f == F_SYM) return office_ttf_advance(w->sym.ttf, gid_in(&w->sym, cp)) * size / 1000;
    return office_ttf_advance(w->font.ttf, gid_new(w, cp)) * size / 1000;
}

/* 行内节点 → 字符数组 */
typedef struct { ch *v; int n, cap; } chars;
static void push(chars *c, ch x) { if (c->n == c->cap) { c->cap = c->cap ? c->cap * 2 : 64; c->v = realloc(c->v, sizeof *c->v * (size_t)c->cap); } c->v[c->n++] = x; }
static void flatten(pw *w, const office_node *n, chars *out, double size, bool bold) {
    for (; n; n = n->next) {
        if (n->kind == OFF_TEXT || n->kind == OFF_LINK) {
            int link = -1;
            if (n->kind == OFF_LINK) { w->links = realloc(w->links, sizeof *w->links * (size_t)(w->nlinks + 1)); w->links[w->nlinks] = strdup(n->url); link = w->nlinks++; }
            for (const char *p = n->text ? n->text : ""; *p; ) { uint32_t cp; p += office_utf8_next(p, &cp); if (cp == '\n') cp = ' '; const bool cd = n->code; push(out, (ch){ cp, cw(w, cp, size, cd), bold || n->bold, n->italic, cd, link }); }
        } else if (n->kind == OFF_IMAGE) { for (const char *p = "[image]"; *p; p++) push(out, (ch){ (uint32_t)*p, cw(w, (uint32_t)*p, size, false), 0, 1, 0, -1 }); }
        else if (n->kind == OFF_BREAK) push(out, (ch){ '\n', 0, 0, 0, 0, -1 });
    }
}

/* 内容流里的一个文本段: 同样式的连续字符 */
static void emit_run(pw *w, const ch *c, int n, double x, double y, double size) {
    ds4_buf *b = &page(w)->content;
    const int f = font_for(w, c->cp, c->code); const bool courier = f == F_COUR, helv = f == F_HELV;
    if (c->code) { double tw = 0; for (int i = 0; i < n; i++) tw += c[i].w; ds4_buf_printf(b, "0.95 g %.2f %.2f %.2f %.2f re f 0 g\n", x - 1, y - size * 0.25, tw + 2, size * 1.2); }
    ds4_buf_printf(b, "BT /%s %.2f Tf %s1 0 %s 1 %.2f %.2f Tm ", f == F_COUR ? "F2" : f == F_HELV ? "F3" : f == F_SYM ? "F4" : "F1", size, c->bold ? "2 Tr 0.3 w 0 G " : "", c->italic ? "0.21" : "0", x, y);   /* 粗体 = 描边; 描边色要显式归黑, 表格线/分隔线把 G 设成灰了 */
    if (c->link >= 0) ds4_buf_puts(b, "0.02 0.39 0.76 rg ");
    ds4_buf_putc(b, '<');
    for (int i = 0; i < n; i++) {
        if (courier || helv) ds4_buf_printf(b, "%02X", c[i].cp < 256 ? c[i].cp : '?');
        else if (w->builtin) ds4_buf_printf(b, "%04X", c[i].cp > 0xFFFF ? '?' : c[i].cp);
        else if (f == F_SYM) ds4_buf_printf(b, "%04X", w->sym.new_of_old[gid_in(&w->sym, c[i].cp)]);
        else ds4_buf_printf(b, "%04X", w->font.new_of_old[gid_new(w, c[i].cp)]);
    }
    ds4_buf_puts(b, "> Tj ");
    if (c->link >= 0) ds4_buf_puts(b, "0 g ");
    if (c->bold) ds4_buf_puts(b, "0 Tr ");
    ds4_buf_puts(b, "ET\n");
    if (c->link >= 0) {   /* 链接注解 */
        double tw = 0; for (int i = 0; i < n; i++) tw += c[i].w;
        ds4_buf *a = &page(w)->annots;
        ds4_buf_printf(a, "<< /Type /Annot /Subtype /Link /Rect [%.2f %.2f %.2f %.2f] /Border [0 0 0] /A << /S /URI /URI ", x, y - size * 0.25, x + tw, y + size);
        pdf_text_string(a, w->links[c->link]); ds4_buf_puts(a, " >> >> ");
    }
}
static bool same_style(const pw *w, const ch *a, const ch *b) { return a->bold == b->bold && a->italic == b->italic && a->code == b->code && a->link == b->link && font_for(w, a->cp, a->code) == font_for(w, b->cp, b->code); }

/* 把字符数组排进宽 width 的栏, 从 (x, w->y) 起; 返回占用的高度。lines_out 给表格量行数用(不画) */
static double layout_chars(pw *w, const chars *c, double x, double width, double size, bool draw, int *lines_out, const char *color) {
    const double lead = size * LEAD; int lines = 0, i = 0;
    while (i < c->n) {
        /* 找这一行能放多少: 记录最后一个可断点 */
        int j = i, brk = -1; double used = 0;
        while (j < c->n) {
            const ch *k = &c->v[j];
            if (k->cp == '\n') { brk = j; break; }
            if (used + k->w > width + 0.01 && j > i) break;
            used += k->w; j++;
            if (k->cp == ' ' || is_cjk(k->cp) || (j < c->n && is_cjk(c->v[j].cp))) brk = j;
        }
        int end = j;
        if (j < c->n && c->v[j].cp != '\n' && brk > i) end = brk;   /* 在可断点断; 没有可断点就按字断 */
        int next = end; if (next < c->n && (c->v[next].cp == ' ' || c->v[next].cp == '\n')) next++;
        while (end > i && c->v[end - 1].cp == ' ') end--;
        if (draw) {
            if (w->y - lead < PDF_BOTTOM) { new_page(w); }
            const double y = w->y - size;   /* 基线 */
            if (color) ds4_buf_printf(&page(w)->content, "%s rg\n", color);
            double cx = x; int s = i;
            while (s < end) { int e = s + 1; while (e < end && same_style(w, &c->v[s], &c->v[e])) e++; emit_run(w, &c->v[s], e - s, cx, y, size); for (int k = s; k < e; k++) cx += c->v[k].w; s = e; }
            if (color) ds4_buf_puts(&page(w)->content, "0 g\n");
            w->y -= lead;
        }
        lines++;
        if (next == i) next = i + 1;   /* 防死循环 */
        i = next;
    }
    if (!c->n) { lines = 1; if (draw) w->y -= lead; }
    if (lines_out) *lines_out = lines;
    return lines * lead;
}

static void para(pw *w, const office_node *inl, double x, double width, double size, bool bold, double after, const char *color) {
    chars c = {0}; flatten(w, inl, &c, size, bold);
    need(w, size * LEAD * 2);
    layout_chars(w, &c, x, width, size, true, NULL, color);
    w->y -= after; free(c.v);
}
static void heading(pw *w, const office_node *n) {
    static const double sz[] = { 20, 16, 13, 12, 12, 12 }, before[] = { 14, 12, 10, 8, 8, 8 };
    const double size = sz[n->level - 1];
    if (w->y < PDF_TOP - 1) w->y -= before[n->level - 1];
    need(w, size * LEAD * 3);   /* 标题后面至少跟两行 */
    para(w, n->child, PDF_MARGIN, PDF_TEXT_W, size, true, 6, "0.12 0.22 0.39");
}
static void list(pw *w, const office_node *ls, int level) {
    const double x = PDF_MARGIN + IND * level;
    int k = 0;
    for (const office_node *it = ls->child; it; it = it->next, k++) {
        chars m = {0}; char mark[16];
        if (ls->ordered) snprintf(mark, sizeof mark, level > 1 ? "%c)" : "%d.", level > 1 ? 'a' + k % 26 : k + 1); else snprintf(mark, sizeof mark, "%s", level > 1 ? "\xE2\x97\xA6" : "\xE2\x80\xA2");
        for (const char *p = mark; *p; ) { uint32_t cp; p += office_utf8_next(p, &cp); push(&m, (ch){ cp, cw(w, cp, BODY, false), 0, 0, 0, -1 }); }
        need(w, BODY * LEAD * 2);
        const double y0 = w->y;
        layout_chars(w, &m, x, IND, BODY, true, NULL, NULL); w->y = y0; free(m.v);
        chars c = {0}; flatten(w, it->child, &c, BODY, false);   /* flatten 自己走兄弟链并跳过嵌套列表 */
        layout_chars(w, &c, x + IND, PDF_TEXT_W - IND * (level + 1) + IND, BODY, true, NULL, NULL); free(c.v);
        w->y -= 2;
        for (const office_node *x2 = it->child; x2; x2 = x2->next) if (x2->kind == OFF_LIST) list(w, x2, level + 1);
    }
    if (level == 1) w->y -= 6;
}
static void quote(pw *w, const office_node *n) {
    chars c = {0}; flatten(w, n->child, &c, BODY, false); for (int i = 0; i < c.n; i++) c.v[i].italic = 1;
    need(w, BODY * LEAD * 2);
    const double y0 = w->y; int lines;
    layout_chars(w, &c, PDF_MARGIN + 14, PDF_TEXT_W - 14, BODY, true, &lines, "0.35 0.35 0.35");
    if (y0 - w->y > 0 && y0 >= w->y) ds4_buf_printf(&page(w)->content, "0.75 g %.2f %.2f 3 %.2f re f 0 g\n", PDF_MARGIN, w->y + 2, y0 - w->y - 2);   /* 跨页时只画最后一页那段 */
    w->y -= 8; free(c.v);
}
static void code_block(pw *w, const char *text) {
    const double size = 9.5, lead = size * 1.3; const int maxc = (int)(PDF_TEXT_W / (600.0 * size / 1000)) - 2;
    need(w, lead * 2);
    for (const char *p = text; *p; ) {
        const char *nl = strchr(p, '\n'); size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (!nl && !n) break;
        for (size_t off = 0; off == 0 || off < n; ) {   /* 长行按字符数折 */
            size_t take = n - off; chars c = {0}; int cnt = 0; const char *q = p + off;
            while ((size_t)(q - p) < off + take && cnt < maxc) { uint32_t cp; q += office_utf8_next(q, &cp); push(&c, (ch){ cp, cw(w, cp, size, true), 0, 0, 1, -1 }); cnt++; }
            if (w->y - lead < PDF_BOTTOM) new_page(w);
            ds4_buf_printf(&page(w)->content, "0.95 g %.2f %.2f %.2f %.2f re f 0 g\n", PDF_MARGIN, w->y - lead, PDF_TEXT_W, lead);
            double cx = PDF_MARGIN + 6; int s = 0;
            while (s < c.n) { int e = s + 1; while (e < c.n && same_style(w, &c.v[s], &c.v[e])) e++; emit_run(w, &c.v[s], e - s, cx, w->y - size, size); for (int k = s; k < e; k++) cx += c.v[k].w; s = e; }
            w->y -= lead; off = (size_t)(q - p); free(c.v);
            if (!n) break;
        }
        if (!nl) break;
        p = nl + 1;
    }
    w->y -= 8;
}
static void table(pw *w, const office_node *t) {
    const int nc = t->ncols > 0 ? t->ncols : 1; const double colw = PDF_TEXT_W / nc, pad = 4, size = 10;
    for (const office_node *r = t->child; r; r = r->next) {
        chars cells[64]; int lines[64], maxl = 1, k = 0;
        for (const office_node *c = r->child; c && k < nc && k < 64; c = c->next, k++) { cells[k] = (chars){0}; flatten(w, c->child, &cells[k], size, r->header); double sy = w->y; layout_chars(w, &cells[k], 0, colw - 2 * pad, size, false, &lines[k], NULL); w->y = sy; if (lines[k] > maxl) maxl = lines[k]; }
        const double rh = maxl * size * LEAD + 2 * pad;
        if (w->y - rh < PDF_BOTTOM) new_page(w);
        ds4_buf *b = &page(w)->content;
        if (r->header) ds4_buf_printf(b, "0.85 0.89 0.95 rg %.2f %.2f %.2f %.2f re f 0 g\n", PDF_MARGIN, w->y - rh, PDF_TEXT_W, rh);
        ds4_buf_printf(b, "0.55 G 0.5 w %.2f %.2f %.2f %.2f re S\n", PDF_MARGIN, w->y - rh, PDF_TEXT_W, rh);
        for (int i = 1; i < nc; i++) ds4_buf_printf(b, "%.2f %.2f m %.2f %.2f l S\n", PDF_MARGIN + colw * i, w->y, PDF_MARGIN + colw * i, w->y - rh);
        const double y0 = w->y;
        for (int i = 0; i < k; i++) { w->y = y0 - pad; layout_chars(w, &cells[i], PDF_MARGIN + colw * i + pad, colw - 2 * pad, size, true, NULL, NULL); free(cells[i].v); }
        w->y = y0 - rh;
    }
    w->y -= 10;
}
static void image(pw *w, const office_node *im) {
    char path[4096]; office_join(w->o->base_dir, im->url, path, sizeof path);
    size_t n = 0; uint8_t *data = office_slurp(path, &n); office_imginfo info; int obj = 0;
    if (data && office_img_info(data, n, &info)) obj = pdf_image(&w->d, data, n, &info);
    free(data);
    if (!obj) { w->rep->missing_images++; ds4_buf_printf(&w->rep->notes, "image not usable (missing or not png/jpeg): %s\n", im->url); office_node t = { OFF_TEXT, 0, 0, 0, 0, 1, 0, (char *)"[image not found]", NULL, 0, NULL, NULL }; para(w, &t, PDF_MARGIN, PDF_TEXT_W, BODY, false, 6, NULL); return; }
    w->rep->images++; w->nimg++;
    ds4_buf_printf(&w->xobjs, "/Im%d %d 0 R ", w->nimg, obj);
    double dw = info.w * 0.75, dh = info.h * 0.75;   /* 96 dpi → pt */
    if (dw > PDF_TEXT_W) { dh *= PDF_TEXT_W / dw; dw = PDF_TEXT_W; }
    const double maxh = PDF_TOP - PDF_BOTTOM - 10; if (dh > maxh) { dw *= maxh / dh; dh = maxh; }
    if (w->y - dh < PDF_BOTTOM) new_page(w);
    ds4_buf_printf(&page(w)->content, "q %.2f 0 0 %.2f %.2f %.2f cm /Im%d Do Q\n", dw, dh, PDF_MARGIN + (PDF_TEXT_W - dw) / 2, w->y - dh, w->nimg);
    w->y -= dh + 8;
}

static void block(pw *w, const office_node *n) {
    switch (n->kind) {
    case OFF_HEADING: heading(w, n); break;
    case OFF_PARA: para(w, n->child, PDF_MARGIN, PDF_TEXT_W, BODY, false, 8, NULL); break;
    case OFF_LIST: list(w, n, 1); break;
    case OFF_QUOTE: quote(w, n); break;
    case OFF_CODE: code_block(w, n->text ? n->text : ""); break;
    case OFF_TABLE: table(w, n); break;
    case OFF_IMAGE: image(w, n); break;
    case OFF_HR: need(w, 12); ds4_buf_printf(&page(w)->content, "0.75 G 0.5 w %.2f %.2f m %.2f %.2f l S\n", PDF_MARGIN, w->y - 6, PDF_MARGIN + PDF_TEXT_W, w->y - 6); w->y -= 14; break;
    default: break;
    }
}

uint8_t *office_pdf_build(const office_node *doc, const office_opts *o, office_report *rep, size_t *len) {
    pw w; memset(&w, 0, sizeof w); w.o = o; w.rep = rep;
    const char *fp = o->font_path ? o->font_path : office_ttf_find();
    char err[256] = "";
    if (fp && fp[0]) w.font.ttf = office_ttf_load(fp, err, sizeof err);
    if (w.font.ttf) {
        font_init(&w.font);
        const char *sp = o->sym_font_path ? o->sym_font_path : office_ttf_find_sym(fp);
        char serr[256] = "";
        if (sp && sp[0] && (w.sym.ttf = office_ttf_load(sp, serr, sizeof serr))) font_init(&w.sym);
        w.helv_ascii = !office_ttf_gid(w.font.ttf, 'A') || !office_ttf_gid(w.font.ttf, 'a');
    } else {
        w.builtin = true;
        ds4_buf_printf(&rep->notes, "PDF fonts are not embedded (no TrueType CJK font found on this machine%s%s); readers without Chinese fonts show boxes. Install fonts-droid-fallback or wqy-microhei to fix.\n", err[0] ? ": " : "", err);
    }
    new_page(&w);
    for (const office_node *n = doc->child; n; n = n->next) block(&w, n);
    /* 页脚页码 */
    for (int i = 0; i < w.npages; i++) {
        char num[16]; snprintf(num, sizeof num, "%d", i + 1); double tw = 0; for (const char *p = num; *p; p++) tw += cw(&w, (uint32_t)*p, 9, false);
        ds4_buf *b = &w.pages[i].content; ds4_buf_printf(b, "0.5 g BT /%s 9 Tf 1 0 0 1 %.2f %.2f Tm <", w.helv_ascii ? "F3" : "F1", (PDF_PAGE_W - tw) / 2, PDF_MARGIN - 20);
        for (const char *p = num; *p; p++) ds4_buf_printf(b, w.helv_ascii ? "%02X" : "%04X", w.builtin ? (uint32_t)*p : w.helv_ascii ? (uint32_t)*p : (uint32_t)w.font.new_of_old[gid_new(&w, (uint32_t)*p)]);
        ds4_buf_puts(b, "> Tj ET 0 g\n");
    }
    const int f1 = pdf_font_objects(&w.d, &w.font), f2 = pdf_courier(&w.d), f3 = pdf_obj(&w.d, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
    char f4[32] = ""; if (w.sym.ttf && w.sym.nnew > 1) snprintf(f4, sizeof f4, " /F4 %d 0 R", pdf_font_objects(&w.d, &w.sym));   /* 没用到后备就不进文件 */
    ds4_buf res = {0}; ds4_buf_printf(&res, "<< /Font << /F1 %d 0 R /F2 %d 0 R /F3 %d 0 R%s >> /XObject << %s>> >>", f1, f2, f3, f4, w.xobjs.ptr ? w.xobjs.ptr : "");
    char *title = o->title ? strdup(o->title) : office_first_heading(doc);
    uint8_t *out = pdf_finish(&w.d, w.pages, w.npages, res.ptr, title ? title : "", o->now, len);
    free(title); ds4_buf_free(&res); ds4_buf_free(&w.xobjs);
    for (int i = 0; i < w.npages; i++) { ds4_buf_free(&w.pages[i].content); ds4_buf_free(&w.pages[i].annots); }
    free(w.pages); for (int i = 0; i < w.nlinks; i++) free(w.links[i]); free(w.links);
    free(w.font.new_of_old); free(w.font.cp_of_new); office_ttf_free(w.font.ttf);
    free(w.sym.new_of_old); free(w.sym.cp_of_new); office_ttf_free(w.sym.ttf);
    return out;
}
