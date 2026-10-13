/* office_docx.c — 文档树 → word/document.xml + 图片 + rels, 再和包骨架(office_docx_pkg.c)一起打成 zip。
 * 段落/标题/列表/表格/代码/引用/图/分隔线各一个发射函数; 行内节点统一走 run()。
 * 图片: 只认 png/jpeg, 按 96 dpi 折成 EMU, 超过正文宽就等比缩; 读不到的图不中断, 原地写一行 "[image not found]" 并记进 report。
 * 列表: 每个一级列表一套编号(numId), 内层用同一 numId 的 ilvl=1, 外/内层各自有序与否记在 kinds 里给 numbering.xml。 */
#include "office_internal.h"
#include <stdio.h>

#define W_NS "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\""
#define R_NS "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
#define WP_NS "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\""
#define MAX_LISTS 512

typedef struct {
    ds4_buf doc, rels;             /* document.xml 正文 / document.xml.rels 条目 */
    office_zip *zip;
    const office_opts *o;
    office_report *rep;
    int rid, nimg, nlist;
    uint8_t kinds[MAX_LISTS];
    bool png, jpeg;
} dw;

uint8_t *office_slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    ds4_buf b = {0}; char tmp[65536]; size_t k;
    while ((k = fread(tmp, 1, sizeof tmp, f)) > 0) ds4_buf_append(&b, tmp, k);
    fclose(f); *n = b.len; ds4_buf_putc(&b, 0);
    return (uint8_t *)b.ptr;
}
void office_join(const char *base, const char *rel, char *out, size_t n) {
    if (rel[0] == '/' || !base || !base[0]) snprintf(out, n, "%s", rel); else snprintf(out, n, "%s/%s", base, rel);
}
char *office_plain(const office_node *inl) {
    ds4_buf b = {0};
    for (; inl; inl = inl->next) { if (inl->text && (inl->kind == OFF_TEXT || inl->kind == OFF_LINK)) ds4_buf_puts(&b, inl->text); if (inl->child) { char *c = office_plain(inl->child); ds4_buf_puts(&b, c); free(c); } }
    return ds4_buf_take(&b);
}
char *office_first_heading(const office_node *doc) {
    for (const office_node *n = doc->child; n; n = n->next) if (n->kind == OFF_HEADING) return office_plain(n->child);
    return NULL;
}

static void text_run(dw *w, const char *s, bool b, bool i, bool code, const char *style) {
    ds4_buf_puts(&w->doc, "<w:r>");
    if (b || i || code || style) {
        ds4_buf_puts(&w->doc, "<w:rPr>");
        if (style) ds4_buf_printf(&w->doc, "<w:rStyle w:val=\"%s\"/>", style);
        if (code) ds4_buf_puts(&w->doc, "<w:rStyle w:val=\"CodeChar\"/>");
        if (b) ds4_buf_puts(&w->doc, "<w:b/><w:bCs/>");
        if (i) ds4_buf_puts(&w->doc, "<w:i/><w:iCs/>");
        ds4_buf_puts(&w->doc, "</w:rPr>");
    }
    ds4_buf_puts(&w->doc, "<w:t xml:space=\"preserve\">"); office_xml_text(&w->doc, s); ds4_buf_puts(&w->doc, "</w:t></w:r>");
}

static void image(dw *w, const office_node *im) {
    char path[4096]; office_join(w->o->base_dir, im->url, path, sizeof path);
    size_t n = 0; uint8_t *data = office_slurp(path, &n); office_imginfo info;
    if (!data || !office_img_info(data, n, &info)) {
        free(data); w->rep->missing_images++;
        ds4_buf_printf(&w->rep->notes, "image not usable (missing or not png/jpeg): %s\n", im->url);
        char msg[4200]; snprintf(msg, sizeof msg, "[image not found: %s]", im->url);
        text_run(w, msg, false, true, false, NULL);
        return;
    }
    w->rep->images++; w->nimg++; w->rid++;
    if (!strcmp(info.ext, "png")) w->png = true; else w->jpeg = true;
    char name[64]; snprintf(name, sizeof name, "media/image%d.%s", w->nimg, info.ext);
    char part[80]; snprintf(part, sizeof part, "word/%s", name);
    office_zip_add(w->zip, part, data, n); free(data);
    ds4_buf_printf(&w->rels, "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"%s\"/>", w->rid, name);
    long cx = (long)info.w * EMU_PER_PX, cy = (long)info.h * EMU_PER_PX;
    if (cx > DOCX_TEXT_W_EMU) { cy = (long)((double)cy * DOCX_TEXT_W_EMU / (double)cx); cx = DOCX_TEXT_W_EMU; }
    ds4_buf_printf(&w->doc, "<w:r><w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\"><wp:extent cx=\"%ld\" cy=\"%ld\"/><wp:docPr id=\"%d\" name=\"Picture %d\" descr=\"", cx, cy, w->nimg, w->nimg);
    office_xml_text(&w->doc, im->text ? im->text : "");
    ds4_buf_printf(&w->doc, "\"/><a:graphic xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
                            "<pic:pic xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"><pic:nvPicPr><pic:cNvPr id=\"%d\" name=\"image%d.%s\"/><pic:cNvPicPr/></pic:nvPicPr>"
                            "<pic:blipFill><a:blip r:embed=\"rId%d\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>"
                            "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%ld\" cy=\"%ld\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic>"
                            "</a:graphicData></a:graphic></wp:inline></w:drawing></w:r>", w->nimg, w->nimg, info.ext, w->rid, cx, cy);
}

static void inline_one(dw *w, const office_node *n, bool force_bold) {
    if (n->kind == OFF_TEXT) text_run(w, n->text, n->bold || force_bold, n->italic, n->code, NULL);
    else if (n->kind == OFF_LINK) {
        w->rid++;
        ds4_buf_printf(&w->rels, "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink\" Target=\"", w->rid);
        office_xml_text(&w->rels, n->url); ds4_buf_puts(&w->rels, "\" TargetMode=\"External\"/>");
        ds4_buf_printf(&w->doc, "<w:hyperlink r:id=\"rId%d\">", w->rid); text_run(w, n->text, n->bold || force_bold, n->italic, false, "Hyperlink"); ds4_buf_puts(&w->doc, "</w:hyperlink>");
    } else if (n->kind == OFF_IMAGE) image(w, n);
    else if (n->kind == OFF_BREAK) ds4_buf_puts(&w->doc, "<w:r><w:br/></w:r>");
}
static void inlines(dw *w, const office_node *n) { for (; n; n = n->next) inline_one(w, n, false); }

static void para_open(dw *w, const char *style, int num_id, int ilvl) {
    ds4_buf_puts(&w->doc, "<w:p>");
    if (style || num_id) {
        ds4_buf_puts(&w->doc, "<w:pPr>");
        if (style) ds4_buf_printf(&w->doc, "<w:pStyle w:val=\"%s\"/>", style);
        if (num_id) ds4_buf_printf(&w->doc, "<w:numPr><w:ilvl w:val=\"%d\"/><w:numId w:val=\"%d\"/></w:numPr>", ilvl, num_id);
        ds4_buf_puts(&w->doc, "</w:pPr>");
    }
}
#define PARA_CLOSE(w) ds4_buf_puts(&(w)->doc, "</w:p>")

static void list(dw *w, const office_node *ls, int num_id, int ilvl) {
    if (!ilvl) { if (w->nlist >= MAX_LISTS) return; num_id = ++w->nlist; w->kinds[num_id - 1] = ls->ordered ? 1 : 0; }
    else if (ls->ordered) w->kinds[num_id - 1] |= 2;
    for (const office_node *it = ls->child; it; it = it->next) {
        para_open(w, "ListParagraph", num_id, ilvl);
        for (const office_node *c = it->child; c; c = c->next) if (c->kind != OFF_LIST) inline_one(w, c, false);
        PARA_CLOSE(w);
        for (const office_node *c = it->child; c; c = c->next) if (c->kind == OFF_LIST) list(w, c, num_id, 1);
    }
}

static void table(dw *w, const office_node *t) {
    const int nc = t->ncols > 0 ? t->ncols : 1, cw = DOCX_TEXT_W_TWIPS / nc;
    ds4_buf_puts(&w->doc, "<w:tbl><w:tblPr><w:tblStyle w:val=\"TableGrid\"/><w:tblW w:w=\"5000\" w:type=\"pct\"/><w:tblLook w:val=\"04A0\" w:firstRow=\"1\" w:lastRow=\"0\" w:firstColumn=\"1\" w:lastColumn=\"0\" w:noHBand=\"0\" w:noVBand=\"1\"/></w:tblPr><w:tblGrid>");
    for (int i = 0; i < nc; i++) ds4_buf_printf(&w->doc, "<w:gridCol w:w=\"%d\"/>", cw);
    ds4_buf_puts(&w->doc, "</w:tblGrid>");
    for (const office_node *r = t->child; r; r = r->next) {
        ds4_buf_puts(&w->doc, r->header ? "<w:tr><w:trPr><w:tblHeader/></w:trPr>" : "<w:tr>");
        int k = 0;
        for (const office_node *c = r->child; c && k < nc; c = c->next, k++) {
            ds4_buf_printf(&w->doc, "<w:tc><w:tcPr><w:tcW w:w=\"%d\" w:type=\"dxa\"/>%s</w:tcPr><w:p><w:pPr><w:spacing w:after=\"0\" w:line=\"276\" w:lineRule=\"auto\"/></w:pPr>", cw,
                           r->header ? "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"D9E2F3\"/>" : "");
            for (const office_node *x = c->child; x; x = x->next) inline_one(w, x, r->header);
            ds4_buf_puts(&w->doc, "</w:p></w:tc>");
        }
        for (; k < nc; k++) ds4_buf_printf(&w->doc, "<w:tc><w:tcPr><w:tcW w:w=\"%d\" w:type=\"dxa\"/></w:tcPr><w:p/></w:tc>", cw);
        ds4_buf_puts(&w->doc, "</w:tr>");
    }
    ds4_buf_puts(&w->doc, "</w:tbl><w:p><w:pPr><w:spacing w:after=\"0\"/></w:pPr></w:p>");   /* 表后空段: 两张表紧挨着 Word 会把它们合成一张 */
}

static void code(dw *w, const char *text) {
    const char *p = text;
    for (;;) {
        const char *nl = strchr(p, '\n'); const size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (!nl && !n) break;
        char *line = malloc(n + 1); memcpy(line, p, n); line[n] = 0;
        para_open(w, "Code", 0, 0); text_run(w, line, false, false, false, NULL); PARA_CLOSE(w); free(line);
        if (!nl) break;
        p = nl + 1;
    }
}

static void block(dw *w, const office_node *n) {
    switch (n->kind) {
    case OFF_HEADING: { char st[16]; snprintf(st, sizeof st, "Heading%d", n->level < 4 ? n->level : 4); para_open(w, st, 0, 0); inlines(w, n->child); PARA_CLOSE(w); break; }
    case OFF_PARA: para_open(w, NULL, 0, 0); inlines(w, n->child); PARA_CLOSE(w); break;
    case OFF_IMAGE: para_open(w, NULL, 0, 0); ds4_buf_puts(&w->doc, "<w:pPr><w:jc w:val=\"center\"/></w:pPr>"); image(w, n); PARA_CLOSE(w); break;
    case OFF_QUOTE: para_open(w, "Quote", 0, 0); inlines(w, n->child); PARA_CLOSE(w); break;
    case OFF_CODE: code(w, n->text ? n->text : ""); ds4_buf_puts(&w->doc, "<w:p><w:pPr><w:spacing w:after=\"0\"/></w:pPr></w:p>"); break;
    case OFF_LIST: list(w, n, 0, 0); break;
    case OFF_TABLE: table(w, n); break;
    case OFF_HR: ds4_buf_puts(&w->doc, "<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\" w:sz=\"6\" w:space=\"1\" w:color=\"BFBFBF\"/></w:pBdr></w:pPr></w:p>"); break;
    default: break;   /* NOTES: 讲稿备注只有 pptx 用 */
    }
}

uint8_t *office_docx_build(const office_node *doc, const office_opts *o, office_report *rep, size_t *len) {
    office_zip z; office_zip_begin(&z, o->now);
    dw w = { {0}, {0}, &z, o, rep, 3, 0, 0, {0}, false, false };   /* rId1..3 = styles / numbering / footer */
    ds4_buf_puts(&w.doc, XML_HEAD "<w:document " W_NS " " R_NS " " WP_NS "><w:body>");
    for (const office_node *n = doc->child; n; n = n->next) block(&w, n);
    ds4_buf_puts(&w.doc, "<w:sectPr><w:footerReference w:type=\"default\" r:id=\"rId3\"/><w:pgSz w:w=\"11906\" w:h=\"16838\"/>"
                         "<w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\" w:header=\"708\" w:footer=\"708\" w:gutter=\"0\"/></w:sectPr></w:body></w:document>");
    ds4_buf b = {0};
    docx_content_types(&b, w.png, w.jpeg); office_zip_add(&z, "[Content_Types].xml", b.ptr, b.len); b.len = 0;
    docx_root_rels(&b); office_zip_add(&z, "_rels/.rels", b.ptr, b.len); b.len = 0;
    char *title = o->title ? strdup(o->title) : office_first_heading(doc);
    ds4_buf app = {0}; docx_props(&b, &app, title ? title : "", o->now); free(title);
    office_zip_add(&z, "docProps/core.xml", b.ptr, b.len); b.len = 0;
    office_zip_add(&z, "docProps/app.xml", app.ptr, app.len); ds4_buf_free(&app);
    office_zip_add(&z, "word/document.xml", w.doc.ptr, w.doc.len); ds4_buf_free(&w.doc);
    docx_styles(&b); office_zip_add(&z, "word/styles.xml", b.ptr, b.len); b.len = 0;
    docx_numbering(&b, w.kinds, w.nlist); office_zip_add(&z, "word/numbering.xml", b.ptr, b.len); b.len = 0;
    docx_footer(&b); office_zip_add(&z, "word/footer1.xml", b.ptr, b.len); b.len = 0;
    ds4_buf_puts(&b, XML_HEAD "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                     "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
                     "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering\" Target=\"numbering.xml\"/>"
                     "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/footer\" Target=\"footer1.xml\"/>");
    ds4_buf_append(&b, w.rels.ptr, w.rels.len); ds4_buf_puts(&b, "</Relationships>");
    office_zip_add(&z, "word/_rels/document.xml.rels", b.ptr, b.len);
    ds4_buf_free(&b); ds4_buf_free(&w.rels);
    return office_zip_end(&z, len);
}
