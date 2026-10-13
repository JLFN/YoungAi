/* office_xlsx.c — JSON [{name, columns, rows}] → 表 → 公式求值 → xl/worksheets/sheetN.xml, 和包骨架(office_xlsx_pkg.c)一起打成 zip。
 * 格的类型按 JSON 值定: 数就是数; 字符串 "=..." 是公式(存不带 = 的原文), 其它字符串是文本; true/false 写成文本; null 空。
 * 文本用 inlineStr(不建 sharedStrings): 少一个部件、少一遍去重, 文件大一点无所谓 —— 这类表格是给人看的, 不是百万行。
 * 列宽按内容最长算(中日韩一个字算两格), 钳在 8~60; 有表头才冻结首行 + 自动筛选。 */
#include "office_internal.h"
#include <stdio.h>

#define MAX_SHEETS 32
#define MAX_CELLS 2000000   /* 2 百万格 ≈ 内存几十 MB; 再大的表不是模型该一次写出来的 */
#define MAX_NOTES 20

/* ---------------- JSON → office_sheet ---------------- */
typedef struct { office_cell *v; int n, cap; } cellvec;
static void push(cellvec *cv, office_cell c) { if (cv->n == cv->cap) { cv->cap = cv->cap ? cv->cap * 2 : 64; cv->v = realloc(cv->v, sizeof *cv->v * (size_t)cv->cap); } cv->v[cv->n++] = c; }

static bool value_cell(const char **p, office_cell *c) {   /* 一个 JSON 标量 → 格 */
    memset(c, 0, sizeof *c);
    json_ws(p);
    if (**p == '"') {
        char *s; if (!json_string(p, &s)) return false;
        if (s[0] == '=') { c->type = CELL_FORMULA; c->str = strdup(s + 1); free(s); }
        else if (!s[0]) { c->type = CELL_EMPTY; free(s); }
        else { c->type = CELL_STR; c->str = s; }
        return true;
    }
    if (json_lit(p, "null")) return true;
    bool b; if (json_bool(p, &b)) { c->type = CELL_STR; c->str = strdup(b ? "TRUE" : "FALSE"); return true; }
    double d; if (json_number(p, &d)) { c->type = CELL_NUM; c->num = d; return true; }
    return false;
}
/* 一行(JSON 数组)→ 追加格, 返回这一行的格数 */
static bool row_cells(const char **p, cellvec *cv, int *n) {
    *n = 0; json_ws(p);
    if (!json_lit(p, "[")) return false;
    json_ws(p);
    if (json_lit(p, "]")) return true;
    for (;;) {
        office_cell c; if (!value_cell(p, &c)) return false;
        push(cv, c); (*n)++;
        json_ws(p);
        if (json_lit(p, ",")) continue;
        return json_lit(p, "]");
    }
}

static bool parse_sheet(const char **p, office_sheet *s, char *err, size_t errn) {
    memset(s, 0, sizeof *s);
    cellvec header = {0}, body = {0}; int *rowlen = NULL, nrows = 0, cap = 0, maxlen = 0;
    json_ws(p);
    if (!json_lit(p, "{")) { snprintf(err, errn, "each sheet must be an object"); return false; }
    for (;;) {
        json_ws(p);
        if (json_lit(p, "}")) break;
        char *key; if (!json_string(p, &key)) { snprintf(err, errn, "bad key in sheet"); goto fail; }
        json_ws(p); if (!json_lit(p, ":")) { free(key); snprintf(err, errn, "expected :"); goto fail; }
        json_ws(p);
        if (!strcmp(key, "name")) { char *v; if (!json_string(p, &v)) { free(key); snprintf(err, errn, "name must be a string"); goto fail; } free(s->name); s->name = v; }
        else if (!strcmp(key, "columns")) { int n; if (!row_cells(p, &header, &n)) { free(key); snprintf(err, errn, "columns must be an array"); goto fail; } }
        else if (!strcmp(key, "rows")) {
            if (!json_lit(p, "[")) { free(key); snprintf(err, errn, "rows must be an array of arrays"); goto fail; }
            json_ws(p);
            if (!json_lit(p, "]")) for (;;) {
                int n; if (!row_cells(p, &body, &n)) { free(key); snprintf(err, errn, "row %d is not an array of values", nrows + 1); goto fail; }
                if (nrows == cap) { cap = cap ? cap * 2 : 64; rowlen = realloc(rowlen, sizeof *rowlen * (size_t)cap); }
                rowlen[nrows++] = n; if (n > maxlen) maxlen = n;
                json_ws(p);
                if (json_lit(p, ",")) continue;
                if (json_lit(p, "]")) break;
                free(key); snprintf(err, errn, "rows: expected , or ]"); goto fail;
            }
        } else if (!json_skip_value(p)) { free(key); snprintf(err, errn, "bad value for %s", key); goto fail; }
        free(key);
        json_ws(p); json_lit(p, ",");
    }
    const bool has_header = header.n > 0;
    s->header = has_header;
    s->cols = header.n > maxlen ? header.n : maxlen;
    s->rows = nrows + (has_header ? 1 : 0);
    if (s->cols < 1) s->cols = 1;
    if ((long)s->rows * s->cols > MAX_CELLS) { snprintf(err, errn, "sheet too large (%d x %d cells)", s->rows, s->cols); goto fail; }
    s->cells = calloc((size_t)(s->rows * s->cols) + 1, sizeof *s->cells);
    for (int c = 0; c < header.n; c++) s->cells[c] = header.v[c];
    for (int r = 0, k = 0; r < nrows; r++) for (int c = 0; c < rowlen[r]; c++, k++) s->cells[(r + (has_header ? 1 : 0)) * s->cols + c] = body.v[k];
    if (!s->name) s->name = strdup("Sheet1");
    free(header.v); free(body.v); free(rowlen);
    return true;
fail:
    for (int i = 0; i < header.n; i++) free(header.v[i].str);
    for (int i = 0; i < body.n; i++) free(body.v[i].str);
    free(header.v); free(body.v); free(rowlen); free(s->name); memset(s, 0, sizeof *s);
    return false;
}

bool office_xlsx_parse(const char *json, office_sheet **out, int *nsheets, char *err, size_t errn) {
    const char *p = json; *out = NULL; *nsheets = 0;
    json_ws(&p);
    if (!json_lit(&p, "[")) { snprintf(err, errn, "sheets must be a JSON array of sheet objects"); return false; }
    office_sheet *sh = calloc(MAX_SHEETS, sizeof *sh); int n = 0;
    json_ws(&p);
    if (!json_lit(&p, "]")) for (;;) {
        if (n == MAX_SHEETS) { snprintf(err, errn, "at most %d sheets", MAX_SHEETS); goto fail; }
        if (!parse_sheet(&p, &sh[n], err, errn)) goto fail;
        n++;
        json_ws(&p);
        if (json_lit(&p, ",")) continue;
        if (json_lit(&p, "]")) break;
        snprintf(err, errn, "sheets: expected , or ]"); goto fail;
    }
    if (!n) { snprintf(err, errn, "sheets is empty"); goto fail; }
    *out = sh; *nsheets = n;
    return true;
fail:
    for (int i = 0; i < n; i++) office_sheet_free(&sh[i]);
    free(sh); return false;
}

/* ---------------- 表 → sheet XML ---------------- */
static int display_width(const office_cell *c) {   /* 近似字符格数: ASCII 1, 多字节码点 2 */
    char tmp[32]; const char *s = tmp;
    if (c->type == CELL_NUM || (c->type == CELL_FORMULA && c->state == 2)) snprintf(tmp, sizeof tmp, "%.15g", c->num);
    else if (c->type == CELL_STR) s = c->str;
    else if (c->type == CELL_FORMULA) s = c->state == 3 ? c->errv : "0";
    else return 0;
    int w = 0; for (; *s; s++) { const unsigned char u = (unsigned char)*s; if (u < 0x80) w++; else if (u >= 0xC0) w += 2; }
    return w;
}
static void sanitize_name(char *name) {   /* Excel 表名: ≤31 字, 不许 [ ] : * ? / \ */
    for (char *p = name; *p; p++) if (strchr("[]:*?/\\", *p)) *p = '_';
    int cps = 0; for (char *p = name; *p; p++) { if (((unsigned char)*p & 0xC0) != 0x80) cps++; if (cps > 31) { *p = 0; break; } }
    if (!name[0]) strcpy(name, "Sheet");
}

static void sheet_xml(ds4_buf *b, office_sheet *s, bool first, bool has_header, office_report *rep) {
    char last[8], ref[16]; office_calc_colname(s->cols - 1, last);
    ds4_buf_printf(b, XML_HEAD "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
                      "<dimension ref=\"A1:%s%d\"/><sheetViews><sheetView workbookViewId=\"0\"%s>", last, s->rows, first ? " tabSelected=\"1\"" : "");
    if (has_header) ds4_buf_puts(b, "<pane ySplit=\"1\" topLeftCell=\"A2\" activePane=\"bottomLeft\" state=\"frozen\"/><selection pane=\"bottomLeft\" activeCell=\"A2\" sqref=\"A2\"/>");
    ds4_buf_puts(b, "</sheetView></sheetViews><sheetFormatPr defaultRowHeight=\"15\"/><cols>");
    for (int c = 0; c < s->cols; c++) {
        int w = 0; for (int r = 0; r < s->rows; r++) { const int k = display_width(&s->cells[r * s->cols + c]); if (k > w) w = k; }
        w += 2; if (w < 8) w = 8; if (w > 60) w = 60;
        ds4_buf_printf(b, "<col min=\"%d\" max=\"%d\" width=\"%d\" customWidth=\"1\"/>", c + 1, c + 1, w);
    }
    ds4_buf_puts(b, "</cols><sheetData>");
    for (int r = 0; r < s->rows; r++) {
        ds4_buf_printf(b, "<row r=\"%d\">", r + 1);
        for (int c = 0; c < s->cols; c++) {
            office_cell *x = &s->cells[r * s->cols + c];
            if (x->type == CELL_EMPTY) continue;
            char col[8]; office_calc_colname(c, col); snprintf(ref, sizeof ref, "%s%d", col, r + 1);
            const char *style = has_header && r == 0 ? " s=\"1\"" : "";
            if (x->type == CELL_NUM) ds4_buf_printf(b, "<c r=\"%s\"%s><v>%.15g</v></c>", ref, style, x->num);
            else if (x->type == CELL_STR) { ds4_buf_printf(b, "<c r=\"%s\"%s t=\"inlineStr\"><is><t xml:space=\"preserve\">", ref, style); office_xml_text(b, x->str); ds4_buf_puts(b, "</t></is></c>"); }
            else {
                rep->formulas++;
                ds4_buf_printf(b, "<c r=\"%s\"%s%s><f>", ref, style, x->state == 3 ? " t=\"e\"" : ""); office_xml_text(b, x->str); ds4_buf_puts(b, "</f>");
                if (x->state == 2) ds4_buf_printf(b, "<v>%.15g</v>", x->num);
                else if (x->state == 3) ds4_buf_printf(b, "<v>%s</v>", x->errv);
                else { rep->uncomputed++; if (rep->uncomputed <= MAX_NOTES) ds4_buf_printf(&rep->notes, "%s!%s =%s is not computed here (Excel will compute it on open)\n", s->name, ref, x->str); }
                ds4_buf_puts(b, "</c>");
            }
        }
        ds4_buf_puts(b, "</row>");
    }
    ds4_buf_puts(b, "</sheetData>");
    if (has_header) ds4_buf_printf(b, "<autoFilter ref=\"A1:%s%d\"/>", last, s->rows);
    ds4_buf_puts(b, "<pageMargins left=\"0.7\" right=\"0.7\" top=\"0.75\" bottom=\"0.75\" header=\"0.3\" footer=\"0.3\"/></worksheet>");
}

uint8_t *office_xlsx_build(const char *sheets_json, time_t now, office_report *rep, size_t *len, char *err, size_t errn) {
    office_sheet *sh; int n;
    if (!office_xlsx_parse(sheets_json, &sh, &n, err, errn)) return NULL;
    office_zip z; office_zip_begin(&z, now);
    ds4_buf b = {0}, rels = {0};
    xlsx_content_types(&b, n); office_zip_add(&z, "[Content_Types].xml", b.ptr, b.len); b.len = 0;
    xlsx_root_rels(&b); office_zip_add(&z, "_rels/.rels", b.ptr, b.len); b.len = 0;
    xlsx_props(&b, &rels, now); office_zip_add(&z, "docProps/core.xml", b.ptr, b.len); office_zip_add(&z, "docProps/app.xml", rels.ptr, rels.len); b.len = 0; rels.len = 0;
    for (int i = 0; i < n; i++) {
        sanitize_name(sh[i].name);
        for (int j = 0; j < i; j++) if (!strcmp(sh[j].name, sh[i].name)) { char tmp[64]; snprintf(tmp, sizeof tmp, "%.28s%d", sh[i].name, i + 1); free(sh[i].name); sh[i].name = strdup(tmp); }
        for (int k = 0; k < sh[i].rows * sh[i].cols; k++) if (sh[i].cells[k].type == CELL_FORMULA) office_calc_eval(&sh[i], k / sh[i].cols, k % sh[i].cols);
        sheet_xml(&b, &sh[i], i == 0, sh[i].header, rep);
        char part[64]; snprintf(part, sizeof part, "xl/worksheets/sheet%d.xml", i + 1);
        office_zip_add(&z, part, b.ptr, b.len); b.len = 0;
    }
    xlsx_workbook(&b, &rels, sh, n);
    office_zip_add(&z, "xl/workbook.xml", b.ptr, b.len); b.len = 0;
    office_zip_add(&z, "xl/_rels/workbook.xml.rels", rels.ptr, rels.len);
    xlsx_styles(&b); office_zip_add(&z, "xl/styles.xml", b.ptr, b.len);
    ds4_buf_free(&b); ds4_buf_free(&rels);
    for (int i = 0; i < n; i++) office_sheet_free(&sh[i]);
    free(sh);
    return office_zip_end(&z, len);
}
