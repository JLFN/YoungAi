/* office_pptx.c — 文档树 → 幻灯片: 按 --- 切片, 每片按内容判版式, 文本框显式定位(不靠占位符), 和包骨架(office_pptx_pkg.c)一起打成 zip。
 * 版式判定(方案稿 §1.1 承诺给模型的规则):
 *   只有一个标题(可带一段话)且是第一页 → 标题页(大字居中 + 副标题); 只有标题 → 章节页(居中);
 *   标题 + 内容 → 内容页: 标题在上, 正文框在下; 有图 → 正文左半、图右半(等比缩进框); 有表 → 表格占正文区(有文字时文字在上三分之一);
 *   代码块 = 等宽浅底段落; 引用 = 斜体灰字; 列表 = 项目符号(两级缩进); 后续标题 = 加粗段落。
 * <!-- --> 注释 = 讲稿备注(notesSlide); 图片只认 png/jpeg, 读不到的图在正文里写一行并记进 report。 */
#include "office_internal.h"
#include <stdio.h>

#define SLIDE_W 12192000
#define SLIDE_H 6858000
#define MARGIN 838200
#define TITLE_Y 365125
#define TITLE_H 1100000
#define BODY_Y 1650000
#define BODY_H 4700000
#define BODY_W (SLIDE_W - 2 * MARGIN)
#define A_NS "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
#define R_NS "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
#define P_NS "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\""

typedef struct { office_zip *zip; const office_opts *o; office_report *rep; ds4_buf xml, rels; int slide, nid, rid, nimg; bool png, jpeg; } sw;

/* 一段(a:p): 行内节点 → runs; marL/lvl 给列表缩进; bullet 给项目符号字符 */
static void run_props(ds4_buf *b, int sz, bool bold, bool italic, bool code, const char *color) {
    ds4_buf_printf(b, "<a:rPr lang=\"zh-CN\" sz=\"%d\"%s%s>", sz, bold ? " b=\"1\"" : "", italic ? " i=\"1\"" : "");
    if (color) ds4_buf_printf(b, "<a:solidFill><a:srgbClr val=\"%s\"/></a:solidFill>", color);
    if (code) ds4_buf_puts(b, "<a:latin typeface=\"Consolas\"/><a:ea typeface=\"Microsoft YaHei\"/>");
    ds4_buf_puts(b, "</a:rPr>");
}
static void inlines(ds4_buf *b, const office_node *n, int sz, bool bold, bool italic, const char *color) {
    for (; n; n = n->next) {
        if (n->kind == OFF_TEXT || n->kind == OFF_LINK) {
            ds4_buf_puts(b, "<a:r>"); run_props(b, sz, bold || n->bold, italic || n->italic, n->code, n->kind == OFF_LINK ? "0563C1" : color);
            ds4_buf_puts(b, "<a:t>"); office_xml_text(b, n->text ? n->text : ""); ds4_buf_puts(b, "</a:t></a:r>");
        } else if (n->kind == OFF_IMAGE) { ds4_buf_puts(b, "<a:r>"); run_props(b, sz, false, true, false, color); ds4_buf_puts(b, "<a:t>["); office_xml_text(b, n->text ? n->text : "image"); ds4_buf_puts(b, "]</a:t></a:r>"); }
        else if (n->kind == OFF_BREAK) ds4_buf_puts(b, "<a:br/>");
    }
}
static void para(ds4_buf *b, const office_node *inl, int sz, bool bold, bool italic, const char *color, int lvl, const char *bullet, const char *align) {
    ds4_buf_puts(b, "<a:p><a:pPr");
    if (align) ds4_buf_printf(b, " algn=\"%s\"", align);
    if (bullet) ds4_buf_printf(b, " marL=\"%d\" indent=\"-285750\" lvl=\"%d\"", 342900 + 457200 * lvl, lvl);
    ds4_buf_puts(b, "><a:spcBef><a:spcPts val=\"600\"/></a:spcBef>");
    if (bullet) ds4_buf_printf(b, "<a:buFont typeface=\"Arial\"/><a:buChar char=\"%s\"/>", bullet); else ds4_buf_puts(b, "<a:buNone/>");
    ds4_buf_puts(b, "</a:pPr>");
    if (inl) inlines(b, inl, sz, bold, italic, color);
    else { ds4_buf_puts(b, "<a:endParaRPr lang=\"zh-CN\""); ds4_buf_printf(b, " sz=\"%d\"/>", sz); }
    ds4_buf_puts(b, "</a:p>");
}
static void text_box(sw *w, long x, long y, long cx, long cy, const char *anchor, const ds4_buf *paras, const char *fill) {
    ds4_buf_printf(&w->xml, "<p:sp><p:nvSpPr><p:cNvPr id=\"%d\" name=\"Text %d\"/><p:cNvSpPr txBox=\"1\"/><p:nvPr/></p:nvSpPr><p:spPr><a:xfrm><a:off x=\"%ld\" y=\"%ld\"/><a:ext cx=\"%ld\" cy=\"%ld\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>", w->nid, w->nid, x, y, cx, cy);
    w->nid++;
    if (fill) ds4_buf_printf(&w->xml, "<a:solidFill><a:srgbClr val=\"%s\"/></a:solidFill>", fill);
    ds4_buf_printf(&w->xml, "</p:spPr><p:txBody><a:bodyPr wrap=\"square\" anchor=\"%s\" lIns=\"91440\" rIns=\"91440\"><a:normAutofit/></a:bodyPr><a:lstStyle/>", anchor);
    ds4_buf_append(&w->xml, paras->ptr, paras->len);
    ds4_buf_puts(&w->xml, "</p:txBody></p:sp>");
}
static void picture(sw *w, const office_node *im, long bx, long by, long bw, long bh) {
    char path[4096]; office_join(w->o->base_dir, im->url, path, sizeof path);
    size_t n = 0; uint8_t *data = office_slurp(path, &n); office_imginfo info;
    if (!data || !office_img_info(data, n, &info)) { free(data); w->rep->missing_images++; ds4_buf_printf(&w->rep->notes, "image not usable (missing or not png/jpeg): %s\n", im->url); return; }
    w->rep->images++; w->nimg++; w->rid++;
    if (!strcmp(info.ext, "png")) w->png = true; else w->jpeg = true;
    char part[80]; snprintf(part, sizeof part, "ppt/media/image%d.%s", w->nimg, info.ext);
    office_zip_add(w->zip, part, data, n); free(data);
    ds4_buf_printf(&w->rels, "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"../media/image%d.%s\"/>", w->rid, w->nimg, info.ext);
    double sx = (double)bw / info.w, sy = (double)bh / info.h, s = sx < sy ? sx : sy;   /* 等比缩进框, 居中 */
    const long cw = (long)(info.w * s), ch = (long)(info.h * s);
    ds4_buf_printf(&w->xml, "<p:pic><p:nvPicPr><p:cNvPr id=\"%d\" name=\"Picture %d\" descr=\"", w->nid, w->nimg); office_xml_text(&w->xml, im->text ? im->text : "");
    ds4_buf_printf(&w->xml, "\"/><p:cNvPicPr><a:picLocks noChangeAspect=\"1\"/></p:cNvPicPr><p:nvPr/></p:nvPicPr><p:blipFill><a:blip r:embed=\"rId%d\"/><a:stretch><a:fillRect/></a:stretch></p:blipFill>"
                            "<p:spPr><a:xfrm><a:off x=\"%ld\" y=\"%ld\"/><a:ext cx=\"%ld\" cy=\"%ld\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>", w->rid, bx + (bw - cw) / 2, by + (bh - ch) / 2, cw, ch);
    w->nid++;
}
static void table(sw *w, const office_node *t, long x, long y, long cx, long cy) {
    const int nc = t->ncols > 0 ? t->ncols : 1, nr = office_count(t, OFF_ROW); const long colw = cx / nc;
    const long rowh = nr && cy / nr < 457200 ? (cy / nr > 300000 ? cy / nr : 300000) : 457200;   /* 行高 0.5 英寸(14 pt 一行正好), 行多才压缩; 不按正文区拉满 */
    ds4_buf_printf(&w->xml, "<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id=\"%d\" name=\"Table %d\"/><p:cNvGraphicFramePr><a:graphicFrameLocks noGrp=\"1\"/></p:cNvGraphicFramePr><p:nvPr/></p:nvGraphicFramePr>"
                            "<p:xfrm><a:off x=\"%ld\" y=\"%ld\"/><a:ext cx=\"%ld\" cy=\"%ld\"/></p:xfrm><a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/table\"><a:tbl><a:tblPr firstRow=\"1\" bandRow=\"1\"/><a:tblGrid>", w->nid, w->nid, x, y, cx, rowh * nr);
    w->nid++;
    for (int i = 0; i < nc; i++) ds4_buf_printf(&w->xml, "<a:gridCol w=\"%ld\"/>", colw);
    ds4_buf_puts(&w->xml, "</a:tblGrid>");
    for (const office_node *r = t->child; r; r = r->next) {
        ds4_buf_printf(&w->xml, "<a:tr h=\"%ld\">", rowh);
        int k = 0;
        for (const office_node *c = r->child; c && k < nc; c = c->next, k++) {
            ds4_buf_puts(&w->xml, "<a:tc><a:txBody><a:bodyPr/><a:lstStyle/>");
            para(&w->xml, c->child, 1400, r->header, false, NULL, 0, NULL, NULL);
            ds4_buf_printf(&w->xml, "</a:txBody><a:tcPr>%s</a:tcPr></a:tc>", r->header ? "<a:solidFill><a:srgbClr val=\"D9E2F3\"/></a:solidFill>" : "");
        }
        for (; k < nc; k++) ds4_buf_puts(&w->xml, "<a:tc><a:txBody><a:bodyPr/><a:lstStyle/><a:p><a:endParaRPr lang=\"zh-CN\"/></a:p></a:txBody><a:tcPr/></a:tc>");
        ds4_buf_puts(&w->xml, "</a:tr>");
    }
    ds4_buf_puts(&w->xml, "</a:tbl></a:graphicData></a:graphic></p:graphicFrame>");
}
static void list_paras(ds4_buf *b, const office_node *ls, int lvl, int sz) {
    int k = 0; char mark[8];
    for (const office_node *it = ls->child; it; it = it->next, k++) {
        if (ls->ordered) snprintf(mark, sizeof mark, "%d.", k + 1);
        office_node head = *it; head.child = NULL;   /* 项的行内部分(不含嵌套列表) */
        ds4_buf_puts(b, "<a:p><a:pPr"); ds4_buf_printf(b, " marL=\"%d\" indent=\"-285750\" lvl=\"%d\"><a:spcBef><a:spcPts val=\"600\"/></a:spcBef>", 342900 + 457200 * lvl, lvl);
        if (ls->ordered) ds4_buf_puts(b, "<a:buFont typeface=\"+mj-lt\"/><a:buAutoNum type=\"arabicPeriod\"/>"); else ds4_buf_printf(b, "<a:buFont typeface=\"Arial\"/><a:buChar char=\"%s\"/>", lvl ? "\xE2\x80\x93" : "\xE2\x80\xA2");
        ds4_buf_puts(b, "</a:pPr>");
        bool any = false;
        for (const office_node *c = it->child; c; c = c->next) if (c->kind != OFF_LIST) { office_node one = *c; one.next = NULL; inlines(b, &one, sz, false, false, NULL); any = true; }
        if (!any) ds4_buf_printf(b, "<a:endParaRPr lang=\"zh-CN\" sz=\"%d\"/>", sz);
        ds4_buf_puts(b, "</a:p>");
        for (const office_node *c = it->child; c; c = c->next) if (c->kind == OFF_LIST) list_paras(b, c, lvl + 1 < 4 ? lvl + 1 : 3, sz > 1400 ? sz - 200 : sz);
        (void)head;
    }
}
/* 正文块 → 段落; 图和表不进这里(单独定位) */
static int body_paras(ds4_buf *b, const office_node *blocks, int sz, const office_node *title) {
    int n = 0;
    for (const office_node *x = blocks; x; x = x->next) {
        if (x == title || x->kind == OFF_IMAGE || x->kind == OFF_TABLE || x->kind == OFF_NOTES || x->kind == OFF_HR) continue;
        n++;
        if (x->kind == OFF_HEADING) para(b, x->child, sz + 200, true, false, "1F3864", 0, NULL, NULL);
        else if (x->kind == OFF_PARA) para(b, x->child, sz, false, false, NULL, 0, NULL, NULL);
        else if (x->kind == OFF_QUOTE) para(b, x->child, sz, false, true, "595959", 0, NULL, NULL);
        else if (x->kind == OFF_LIST) list_paras(b, x, 0, sz);
        else if (x->kind == OFF_CODE) {
            for (const char *p = x->text ? x->text : ""; *p; ) {
                const char *nl = strchr(p, '\n'); const size_t len = nl ? (size_t)(nl - p) : strlen(p);
                if (!nl && !len) break;
                office_node t = { OFF_TEXT, 0, 0, 0, 0, 0, 1, NULL, NULL, 0, NULL, NULL }; char *line = malloc(len + 1); memcpy(line, p, len); line[len] = 0; t.text = line;
                ds4_buf_puts(b, "<a:p><a:pPr><a:spcBef><a:spcPts val=\"0\"/></a:spcBef><a:buNone/></a:pPr>"); inlines(b, &t, sz - 400 > 1000 ? sz - 400 : 1000, false, false, NULL); ds4_buf_puts(b, "</a:p>");
                free(line); if (!nl) break; p = nl + 1;
            }
        }
    }
    return n;
}

static void slide(sw *w, const office_node *blocks, int nblocks, bool first) {
    const office_node *title = NULL, *img = NULL, *tbl = NULL; int nothers = 0; ds4_buf notes = {0};
    for (const office_node *x = blocks; x; x = x->next) {
        if (x->kind == OFF_HEADING && !title && x->level <= 2) title = x;
        else if (x->kind == OFF_IMAGE && !img) img = x;
        else if (x->kind == OFF_TABLE && !tbl) tbl = x;
        else if (x->kind == OFF_NOTES) { if (notes.len) ds4_buf_putc(&notes, '\n'); ds4_buf_puts(&notes, x->text ? x->text : ""); }
        else if (x->kind != OFF_HR) nothers++;
    }
    w->xml.len = 0; w->rels.len = 0; w->nid = 2; w->rid = 1;
    ds4_buf_puts(&w->xml, XML_HEAD "<p:sld " A_NS " " R_NS " " P_NS "><p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/><a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>");
    ds4_buf_puts(&w->rels, XML_HEAD "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout\" Target=\"../slideLayouts/slideLayout1.xml\"/>");
    ds4_buf p = {0};
    const bool only_title = title && !img && !tbl && nothers == 0;
    const office_node *sub = NULL; for (const office_node *x = blocks; x; x = x->next) if (x != title && x->kind == OFF_PARA) { sub = x; break; }
    if (title && !img && !tbl && first && nothers <= 1 && (!nothers || sub)) {   /* 标题页 */
        para(&p, title->child, 4400, true, false, "1F3864", 0, NULL, "ctr");
        text_box(w, MARGIN, 2000000, BODY_W, 1600000, "b", &p, NULL); p.len = 0;
        if (sub) { para(&p, sub->child, 2400, false, false, "595959", 0, NULL, "ctr"); text_box(w, MARGIN, 3700000, BODY_W, 1200000, "t", &p, NULL); p.len = 0; }
    } else if (only_title) {   /* 章节页 */
        para(&p, title->child, 4000, true, false, "1F3864", 0, NULL, "ctr");
        text_box(w, MARGIN, 2400000, BODY_W, 2000000, "ctr", &p, NULL); p.len = 0;
    } else {
        if (title) { para(&p, title->child, 3200, true, false, "1F3864", 0, NULL, NULL); text_box(w, MARGIN, TITLE_Y, BODY_W, TITLE_H, "b", &p, NULL); p.len = 0; }
        const long by = title ? BODY_Y : TITLE_Y, bh = title ? BODY_H : BODY_H + BODY_Y - TITLE_Y;
        const long tw = img ? BODY_W / 2 - 150000 : BODY_W;
        const int sz = nothers > 8 ? 1600 : nothers > 5 ? 1800 : 2000;
        const int np = body_paras(&p, blocks, sz, title);
        if (tbl) {
            if (np) { text_box(w, MARGIN, by, tw, bh / 3, "t", &p, NULL); table(w, tbl, MARGIN, by + bh / 3 + 100000, tw, bh * 2 / 3 - 100000); }
            else table(w, tbl, MARGIN, by, tw, bh);
        } else if (np || !img) text_box(w, MARGIN, by, tw, bh, "t", &p, NULL);
        p.len = 0;
        if (img) picture(w, img, img && (np || tbl) ? MARGIN + BODY_W / 2 + 150000 : MARGIN, by, img && (np || tbl) ? BODY_W / 2 - 150000 : BODY_W, bh);
    }
    ds4_buf_free(&p);
    ds4_buf_puts(&w->xml, "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sld>");
    if (notes.len) { w->rid++; ds4_buf_printf(&w->rels, "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/notesSlide\" Target=\"../notesSlides/notesSlide%d.xml\"/>", w->rid, w->slide); }
    ds4_buf_puts(&w->rels, "</Relationships>");
    char name[64]; snprintf(name, sizeof name, "ppt/slides/slide%d.xml", w->slide); office_zip_add(w->zip, name, w->xml.ptr, w->xml.len);
    snprintf(name, sizeof name, "ppt/slides/_rels/slide%d.xml.rels", w->slide); office_zip_add(w->zip, name, w->rels.ptr, w->rels.len);
    if (notes.len) {
        ds4_buf nb = {0}, nr = {0}; pptx_notes_slide(&nb, &nr, w->slide, notes.ptr);
        snprintf(name, sizeof name, "ppt/notesSlides/notesSlide%d.xml", w->slide); office_zip_add(w->zip, name, nb.ptr, nb.len);
        snprintf(name, sizeof name, "ppt/notesSlides/_rels/notesSlide%d.xml.rels", w->slide); office_zip_add(w->zip, name, nr.ptr, nr.len);
        ds4_buf_free(&nb); ds4_buf_free(&nr);
    }
    ds4_buf_free(&notes);
    (void)nblocks;
}

uint8_t *office_pptx_build(const office_node *doc, const office_opts *o, office_report *rep, size_t *len) {
    office_zip z; office_zip_begin(&z, o->now);
    sw w = { &z, o, rep, {0}, {0}, 0, 2, 1, 0, false, false };
    /* 按 --- 切片: 把链表临时断开成每片一段 */
    office_node *head = doc->child; bool any_notes = false;
    while (head) {
        office_node *cut = head, *prev = NULL; int n = 0;
        while (cut && cut->kind != OFF_HR) { if (cut->kind == OFF_NOTES) any_notes = true; prev = cut; cut = cut->next; n++; }
        office_node *next = cut ? cut->next : NULL;
        if (prev) prev->next = NULL;
        if (n) { w.slide++; slide(&w, head, n, w.slide == 1); }
        if (prev) prev->next = cut;
        head = next;
    }
    if (!w.slide) { w.slide = 1; office_node empty = { OFF_PARA, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, NULL, NULL }; slide(&w, &empty, 1, true); }
    ds4_buf b = {0}, r = {0};
    pptx_content_types(&b, w.slide, any_notes, w.png, w.jpeg); office_zip_add(&z, "[Content_Types].xml", b.ptr, b.len); b.len = 0;
    pptx_root_rels(&b); office_zip_add(&z, "_rels/.rels", b.ptr, b.len); b.len = 0;
    pptx_presentation(&b, &r, w.slide, any_notes); office_zip_add(&z, "ppt/presentation.xml", b.ptr, b.len); office_zip_add(&z, "ppt/_rels/presentation.xml.rels", r.ptr, r.len); b.len = 0; r.len = 0;
    ds4_buf pp = {0}, vp = {0}, ts = {0}; pptx_small_parts(&pp, &vp, &ts);
    office_zip_add(&z, "ppt/presProps.xml", pp.ptr, pp.len); office_zip_add(&z, "ppt/viewProps.xml", vp.ptr, vp.len); office_zip_add(&z, "ppt/tableStyles.xml", ts.ptr, ts.len);
    ds4_buf_free(&pp); ds4_buf_free(&vp); ds4_buf_free(&ts);
    pptx_theme(&b, "DwarfStar"); office_zip_add(&z, "ppt/theme/theme1.xml", b.ptr, b.len); b.len = 0;
    pptx_master(&b, &r); office_zip_add(&z, "ppt/slideMasters/slideMaster1.xml", b.ptr, b.len); office_zip_add(&z, "ppt/slideMasters/_rels/slideMaster1.xml.rels", r.ptr, r.len); b.len = 0; r.len = 0;
    pptx_layout(&b, &r); office_zip_add(&z, "ppt/slideLayouts/slideLayout1.xml", b.ptr, b.len); office_zip_add(&z, "ppt/slideLayouts/_rels/slideLayout1.xml.rels", r.ptr, r.len); b.len = 0; r.len = 0;
    if (any_notes) {
        pptx_theme(&b, "DwarfStar Notes"); office_zip_add(&z, "ppt/theme/theme2.xml", b.ptr, b.len); b.len = 0;
        pptx_notes_master(&b, &r); office_zip_add(&z, "ppt/notesMasters/notesMaster1.xml", b.ptr, b.len); office_zip_add(&z, "ppt/notesMasters/_rels/notesMaster1.xml.rels", r.ptr, r.len); b.len = 0; r.len = 0;
    }
    char *title = o->title ? strdup(o->title) : office_first_heading(doc);
    pptx_props(&b, &r, title ? title : "", w.slide, o->now); office_zip_add(&z, "docProps/core.xml", b.ptr, b.len); office_zip_add(&z, "docProps/app.xml", r.ptr, r.len); free(title);
    ds4_buf_free(&b); ds4_buf_free(&r); ds4_buf_free(&w.xml); ds4_buf_free(&w.rels);
    return office_zip_end(&z, len);
}
