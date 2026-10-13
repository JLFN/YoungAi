/* t_office.c — src/office 的单测(make ds4_office_test && ./ds4_office_test): 不要模型、不要网络、秒级。
 * ① flate: 已知向量("hello" 的 zlib 产物)解得对、随机数据 deflate→inflate 往返相同、CRC32 标准校验值。
 * ② Markdown: 夹具 report.md 的树形(标题/表/列表嵌套/代码/引用/图/注释/行内样式)。
 * ③ 公式: 夹具 sales.json 每个公式的值、错误值、不认识的函数不填值。
 * ④ 金标: report.md + 运行时造的 PNG → docx; sales.json → xlsx; 与 tests/fixtures/office/ 下的 .golden 逐字节同(时间传常量)。
 *    --update-golden 重写金标(改了版式后有意为之才用; 改前先 diff 看变了什么)。
 * ⑤ 外部复验(有就跑, 没有 SKIP): unzip -t 零错; xmllint 每个 XML 部件良构; macOS textutil 从 docx 抽回的正文含夹具里的句子。
 * ⑥ PNG 解码(去滤波)。⑦ PDF(非内嵌路金标; 内嵌路: 子集化、ToUnicode、xref、macOS mdimport 抽文本)+ PPTX(金标; 分页/备注/图/表)。 */
#include "../../src/office/ds4_office.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, passes;
#define CHECK(cond, ...) do { if (cond) passes++; else { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define FIX "tests/fixtures/office/"
#define NOW 1760000000   /* 2025-10-09T08:53:20Z: 常量时间 → 产物逐字节可比 */

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc((size_t)len + 1); *n = fread(p, 1, (size_t)len, f); p[*n] = 0; fclose(f); return p;
}
static bool put(const char *path, const void *p, size_t n) { FILE *f = fopen(path, "wb"); if (!f) return false; fwrite(p, 1, n, f); fclose(f); return true; }

/* ---------------- ① flate ---------------- */
static void test_flate(void) {
    CHECK(office_crc32("123456789", 9) == 0xCBF43926u, "crc32 check value: %08x", office_crc32("123456789", 9));
    static const uint8_t hello[] = { 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00 };   /* zlib.compress(b"hello") 去掉 2 字节头和 4 字节 adler */
    uint8_t *out; size_t n;
    CHECK(office_inflate(hello, sizeof hello, &out, &n) && n == 5 && !memcmp(out, "hello", 5), "inflate known vector"); free(out);
    uint8_t *buf = malloc(200000); uint32_t s = 12345;
    for (int i = 0; i < 200000; i++) { s = s * 1103515245u + 12345u; buf[i] = (uint8_t)((s >> 16) % (i % 3000 < 1500 ? 7 : 251)); }   /* 一半很可压, 一半近随机 */
    uint8_t *def; const size_t dn = office_deflate(buf, 200000, &def);
    CHECK(dn < 200000, "deflate shrinks mixed data: %zu", dn);
    CHECK(office_inflate(def, dn, &out, &n) && n == 200000 && !memcmp(out, buf, n), "deflate→inflate roundtrip 200k (%zu → %zu)", (size_t)200000, dn); free(out); free(def);
    const size_t en = office_deflate(buf, 0, &def); CHECK(office_inflate(def, en, &out, &n) && n == 0, "empty roundtrip"); free(out); free(def);
    for (int i = 0; i < 5000; i++) buf[i] = 'a';
    const size_t rn = office_deflate(buf, 5000, &def); CHECK(rn < 40, "5000 x 'a' → %zu bytes", rn);
    CHECK(office_inflate(def, rn, &out, &n) && n == 5000 && out[4999] == 'a', "run roundtrip"); free(out); free(def);
    static const uint8_t bad[] = { 0x07, 0xff, 0xff };
    CHECK(!office_inflate(bad, sizeof bad, &out, &n), "garbage rejected");
    free(buf);
}

/* ---------------- 造一张 PNG(用我们自己的 deflate + CRC): 4x3 RGB ---------------- */
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void chunk(FILE *f, const char *type, const uint8_t *data, size_t n) {
    uint8_t len[4]; be32(len, (uint32_t)n); fwrite(len, 1, 4, f);
    uint8_t *tmp = malloc(n + 4); memcpy(tmp, type, 4); memcpy(tmp + 4, data, n);
    fwrite(tmp, 1, n + 4, f);
    uint8_t crc[4]; be32(crc, office_crc32(tmp, n + 4)); fwrite(crc, 1, 4, f); free(tmp);
}
static void make_png(const char *path) {
    FILE *f = fopen(path, "wb");
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A }; fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {0}; be32(ihdr, 4); be32(ihdr + 4, 3); ihdr[8] = 8; ihdr[9] = 2; chunk(f, "IHDR", ihdr, 13);
    uint8_t raw[3 * 13]; for (int y = 0; y < 3; y++) { raw[y * 13] = 0; for (int x = 0; x < 12; x++) raw[y * 13 + 1 + x] = (uint8_t)(x * 20 + y * 7); }
    uint8_t *def; const size_t dn = office_deflate(raw, sizeof raw, &def);
    uint8_t *idat = malloc(dn + 6); idat[0] = 0x78; idat[1] = 0x01; memcpy(idat + 2, def, dn);
    uint32_t a = 1, b = 0; for (size_t i = 0; i < sizeof raw; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    be32(idat + 2 + dn, b << 16 | a); chunk(f, "IDAT", idat, dn + 6); free(idat); free(def);
    chunk(f, "IEND", NULL, 0); fclose(f);
}

/* ---------------- ② Markdown ---------------- */
static const office_node *nth(const office_node *n, int kind, int k) { for (; n; n = n->next) if (n->kind == kind && !k--) return n; return NULL; }
static void test_md(void) {
    size_t n; char *md = (char *)slurp(FIX "report.md", &n); CHECK(md, "fixture report.md"); if (!md) return;
    office_node *d = office_md_parse(md);
    CHECK(office_count(d, OFF_HEADING) == 4, "headings: %d", office_count(d, OFF_HEADING));
    const office_node *h = nth(d->child, OFF_HEADING, 0); CHECK(h && h->level == 1 && h->child && !strcmp(h->child->text, "三季度经营分析"), "h1 text");
    const office_node *p = nth(d->child, OFF_PARA, 0);
    CHECK(p && p->child && p->child->next && p->child->next->bold && !strcmp(p->child->next->text, "2026 年 7–9 月"), "bold run: %s", p && p->child && p->child->next ? p->child->next->text : "?");
    int links = 0, codes = 0, italics = 0; for (const office_node *x = p ? p->child : NULL; x; x = x->next) { links += x->kind == OFF_LINK; codes += x->code; italics += x->italic; }
    CHECK(links == 1 && codes == 1 && italics == 1, "inline link/code/italic: %d %d %d", links, codes, italics);
    const office_node *t = nth(d->child, OFF_TABLE, 0); CHECK(t && t->ncols == 4 && office_count(t, OFF_ROW) == 4 && t->child->header, "table 4 cols 4 rows");
    const office_node *l = nth(d->child, OFF_LIST, 0); CHECK(l && !l->ordered && office_count(l, OFF_ITEM) == 5 && office_count(l, OFF_LIST) == 2, "bullet list 3 items + nested 2: items %d lists %d", l ? office_count(l, OFF_ITEM) : -1, l ? office_count(l, OFF_LIST) : -1);
    const office_node *nl = l ? nth(l->child->child, OFF_LIST, 0) : NULL; CHECK(nl && nl->level == 2 && !nl->ordered, "nested list level 2");
    const office_node *ol = nth(d->child, OFF_LIST, 1); CHECK(ol && ol->ordered && office_count(ol, OFF_ITEM) == 5, "ordered list with nested ordered: items %d", ol ? office_count(ol, OFF_ITEM) : -1);
    const office_node *q = nth(d->child, OFF_QUOTE, 0); CHECK(q && q->child && strstr(q->child->text, "风险"), "quote");
    const office_node *c = nth(d->child, OFF_CODE, 0); CHECK(c && c->text && strstr(c->text, "GROUP BY region;\n"), "code block keeps lines");
    const office_node *im = nth(d->child, OFF_IMAGE, 0); CHECK(im && !strcmp(im->url, "chart.png") && !strcmp(im->text, "季度营收走势"), "block image");
    CHECK(nth(d->child, OFF_HR, 0), "hr");
    const office_node *nt = nth(d->child, OFF_NOTES, 0); CHECK(nt && !strcmp(nt->text, "内部备注：数据截止 9 月 28 日"), "notes: %s", nt ? nt->text : "?");
    CHECK(office_count(d, OFF_PARA) == 2, "two top-level paragraphs: %d", office_count(d, OFF_PARA));
    const office_node *last = nth(d->child, OFF_PARA, 1);
    int bold = 0, ital = 0; bool snake = false;
    for (const office_node *x = last ? last->child : NULL; x; x = x->next) { bold += x->bold; ital += x->italic; if (x->text && strstr(x->text, "snake_case_word") && !x->italic) snake = true; }
    CHECK(bold == 1 && ital == 1 && snake, "__bold__ _italic_ and snake_case untouched: %d %d %d", bold, ital, snake);
    office_node *e = office_md_parse("```\nunclosed fence\n\n| a | b |\n|---|\n| 1 |\n- \n");
    CHECK(e && office_count(e, OFF_CODE) == 1, "unclosed fence does not crash"); office_node_free(e);
    office_node_free(d); free(md);
}

/* ---------------- ③ 公式 ---------------- */
static void test_calc(void) {
    size_t n; char *js = (char *)slurp(FIX "sales.json", &n); CHECK(js, "fixture sales.json"); if (!js) return;
    office_sheet *sh; int ns; char err[256] = "";
    CHECK(office_xlsx_parse(js, &sh, &ns, err, sizeof err) && ns == 2, "parse: %s", err); if (!sh) { free(js); return; }
    CHECK(sh[0].rows == 9 && sh[0].cols == 5 && sh[0].header && !sh[1].header && sh[1].rows == 2 && sh[1].cols == 3, "shape %dx%d / %dx%d", sh[0].rows, sh[0].cols, sh[1].rows, sh[1].cols);
    for (int k = 0; k < sh[0].rows * sh[0].cols; k++) if (sh[0].cells[k].type == CELL_FORMULA) office_calc_eval(&sh[0], k / 5, k % 5);
    struct { const char *ref; int state; double v; const char *e; } exp[] = {
        { "D2", 2, 160, 0 }, { "E2", 2, 160.0 / 420, 0 }, { "B5", 2, 1385, 0 }, { "D5", 2, 1385 - 830, 0 }, { "E5", 2, 555.0 / 1385, 0 },
        { "B6", 2, 1385.0 / 3, 0 }, { "E6", 2, (160.0 / 420 + 185.0 / 455 + 210.0 / 510) / 3, 0 }, { "B7", 2, 510, 0 }, { "B8", 2, 3, 0 },
        { "B9", 2, 420.0 / 1385, 0 }, { "C9", 3, 0, "#DIV/0!" }, { "D9", 4, 0, 0 }, { "E9", 2, 1024, 0 },
    };
    for (size_t i = 0; i < sizeof exp / sizeof exp[0]; i++) {
        int r, c; const char *e2; office_calc_ref(exp[i].ref, &r, &c, &e2);
        const office_cell *x = office_cell_at(&sh[0], r, c);
        const bool ok = x && x->type == CELL_FORMULA && x->state == exp[i].state && (exp[i].state != 2 || (x->num > exp[i].v - 1e-9 && x->num < exp[i].v + 1e-9)) && (exp[i].state != 3 || !strcmp(x->errv, exp[i].e));
        CHECK(ok, "%s: state %d num %.6f err %s", exp[i].ref, x ? x->state : -1, x ? x->num : 0, x && x->errv ? x->errv : "-");
    }
    int r, c; const char *e2;
    CHECK(office_calc_ref("$AB$12", &r, &c, &e2) && r == 11 && c == 27 && !*e2, "ref $AB$12 → %d,%d", r, c);
    CHECK(!office_calc_ref("A1B", &r, &c, &e2) && !office_calc_ref("SUM", &r, &c, &e2), "non-refs");
    char col[8]; office_calc_colname(0, col); CHECK(!strcmp(col, "A"), "col A"); office_calc_colname(27, col); CHECK(!strcmp(col, "AB"), "col AB: %s", col); office_calc_colname(702, col); CHECK(!strcmp(col, "AAA"), "col AAA: %s", col);
    /* 环与算术里的文本 */
    office_sheet *s2; int n2; const char *cyc = "[{\"rows\":[[\"=B1+1\",\"=A1+1\",\"=\\\"x\\\"+1\",\"=A2*2\",\"=(1+2)*3-4/2\",\"=-E1\"],[\"abc\"]]}]";
    CHECK(office_xlsx_parse(cyc, &s2, &n2, err, sizeof err), "parse cyc: %s", err);
    for (int k = 0; k < 6; k++) office_calc_eval(&s2[0], 0, k);
    CHECK(s2[0].cells[0].state == 3 && !strcmp(s2[0].cells[0].errv, "#REF!"), "cycle → #REF!: %d %s", s2[0].cells[0].state, s2[0].cells[0].errv ? s2[0].cells[0].errv : "-");
    CHECK(s2[0].cells[2].state == 4, "string literal → not computed: %d", s2[0].cells[2].state);
    CHECK(s2[0].cells[3].state == 3 && !strcmp(s2[0].cells[3].errv, "#VALUE!"), "text in arithmetic → #VALUE!");
    CHECK(s2[0].cells[4].state == 2 && s2[0].cells[4].num == 7, "precedence: %g", s2[0].cells[4].num);
    CHECK(s2[0].cells[5].state == 2 && s2[0].cells[5].num == -7, "unary minus on ref: %g", s2[0].cells[5].num);
    office_sheet_free(&s2[0]); free(s2);
    CHECK(!office_xlsx_parse("{\"a\":1}", &s2, &n2, err, sizeof err) && strstr(err, "array"), "bad top-level: %s", err);
    CHECK(!office_xlsx_parse("[{\"rows\":[1]}]", &s2, &n2, err, sizeof err) && strstr(err, "row 1"), "bad row: %s", err);
    for (int i = 0; i < ns; i++) office_sheet_free(&sh[i]);
    free(sh); free(js);
}

/* ---------------- ④ 金标 + ⑤ 外部复验 ---------------- */
static bool have(const char *prog) { char p[256]; snprintf(p, sizeof p, "/usr/bin/%s", prog); return !access(p, X_OK); }
static void external_checks(const char *file, const char *tmp, const char *must_contain) {
    char cmd[2048];
    if (have("unzip")) {
        snprintf(cmd, sizeof cmd, "/usr/bin/unzip -tq '%s' >/dev/null 2>&1", file); CHECK(!system(cmd), "unzip -t %s", file);
        snprintf(cmd, sizeof cmd, "rm -rf '%s/x' && mkdir -p '%s/x' && /usr/bin/unzip -q -o '%s' -d '%s/x'", tmp, tmp, file, tmp); CHECK(!system(cmd), "unzip extract %s", file);
        if (have("xmllint")) { snprintf(cmd, sizeof cmd, "find '%s/x' \\( -name '*.xml' -o -name '*.rels' \\) -exec /usr/bin/xmllint --noout {} + 2>&1 | head -5", tmp); FILE *f = popen(cmd, "r"); char line[512] = ""; if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; pclose(f); } CHECK(!line[0], "xmllint %s: %s", file, line); }
        else printf("SKIP xmllint (not installed)\n");
    } else printf("SKIP unzip (not installed)\n");
    if (must_contain && have("textutil")) {   /* macOS 自带的 Word 读取器: 抽回正文证明 Word 的解析器认我们的 XML */
        snprintf(cmd, sizeof cmd, "/usr/bin/textutil -convert txt -stdout '%s' 2>/dev/null", file);
        FILE *f = popen(cmd, "r"); ds4_buf b = {0}; char buf[4096]; size_t k; while (f && (k = fread(buf, 1, sizeof buf, f)) > 0) ds4_buf_append(&b, buf, k); if (f) pclose(f); ds4_buf_putc(&b, 0);
        CHECK(b.len > 1 && strstr(b.ptr, must_contain) && strstr(b.ptr, "GROUP BY region") && strstr(b.ptr, "新客户数") && strstr(b.ptr, "季度招标改月度比价"), "textutil roundtrip of %s (%zu bytes)", file, b.len);
        ds4_buf_free(&b);
    } else if (must_contain) printf("SKIP textutil (not macOS)\n");
}
static void golden(const char *name, const uint8_t *p, size_t n, bool update) {
    char gp[512]; snprintf(gp, sizeof gp, FIX "%s.golden", name);
    if (update) { CHECK(put(gp, p, n), "write %s", gp); printf("golden updated: %s (%zu bytes)\n", gp, n); return; }
    size_t gn; uint8_t *g = slurp(gp, &gn);
    CHECK(g, "golden %s exists (run with --update-golden to create)", gp);
    if (g) { size_t i = 0; while (i < n && i < gn && g[i] == p[i]) i++; CHECK(gn == n && i == n, "%s matches golden byte for byte (differs at %zu; sizes %zu vs %zu)", name, i, n, gn); free(g); }
}
static void test_build(bool update) {
    char tmp[256]; snprintf(tmp, sizeof tmp, "/tmp/ds4_office_test_%ld", (long)getpid()); mkdir(tmp, 0755);
    char png[300]; snprintf(png, sizeof png, "%s/chart.png", tmp); make_png(png);
    size_t pn; uint8_t *pb = slurp(png, &pn); office_imginfo ii;
    CHECK(pb && office_img_info(pb, pn, &ii) && ii.w == 4 && ii.h == 3 && !strcmp(ii.ext, "png"), "png info"); free(pb);
    static const uint8_t jpg[] = { 0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0, 0xFF, 0xC0, 0x00, 0x11, 0x08, 0x01, 0xE0, 0x02, 0x80, 0x03 };
    CHECK(office_img_info(jpg, sizeof jpg, &ii) && ii.w == 640 && ii.h == 480 && !strcmp(ii.ext, "jpeg"), "jpeg info %dx%d", ii.w, ii.h);
    size_t n; char *md = (char *)slurp(FIX "report.md", &n);
    office_node *d = office_md_parse(md); office_opts o = { tmp, NULL, NOW, NULL, NULL }; office_report rep = {0}; size_t dn;
    uint8_t *docx = office_docx_build(d, &o, &rep, &dn);
    CHECK(docx && dn > 2000 && rep.images == 1 && !rep.missing_images, "docx built %zu bytes, images %d/%d", dn, rep.images, rep.missing_images);
    CHECK(dn >= 4 && docx[0] == 'P' && docx[1] == 'K', "zip signature");
    golden("report.docx", docx, dn, update);
    char f1[300]; snprintf(f1, sizeof f1, "%s/report.docx", tmp); put(f1, docx, dn);
    external_checks(f1, tmp, "三季度经营分析");
    office_report_free(&rep); office_node_free(d); free(docx); free(md);
    /* 缺图: 不中断, 记 report */
    office_node *d2 = office_md_parse("# T\n\n![x](nope.png)\n"); office_report r2 = {0}; size_t n2;
    uint8_t *b2 = office_docx_build(d2, &o, &r2, &n2);
    CHECK(b2 && r2.missing_images == 1 && strstr(r2.notes.ptr, "nope.png"), "missing image reported: %s", r2.notes.ptr ? r2.notes.ptr : "-");
    free(b2); office_report_free(&r2); office_node_free(d2);
    /* xlsx */
    char *js = (char *)slurp(FIX "sales.json", &n); office_report r3 = {0}; char err[256] = ""; size_t xn;
    uint8_t *xlsx = office_xlsx_build(js, NOW, &r3, &xn, err, sizeof err);
    CHECK(xlsx && r3.formulas == 23 && r3.uncomputed == 1 && strstr(r3.notes.ptr, "销售!D9 =VLOOKUP"), "xlsx built: formulas %d uncomputed %d notes %s", r3.formulas, r3.uncomputed, r3.notes.ptr ? r3.notes.ptr : "-");
    golden("sales.xlsx", xlsx, xn, update);
    char f2[300]; snprintf(f2, sizeof f2, "%s/sales.xlsx", tmp); put(f2, xlsx, xn);
    external_checks(f2, tmp, NULL);
    /* 第二张表名里的非法字符被换掉, 没表头的表没有冻结与筛选 */
    char cmd[1024]; snprintf(cmd, sizeof cmd, "%s/x/xl/workbook.xml", tmp); size_t wn; char *wb = (char *)slurp(cmd, &wn);
    CHECK(wb && strstr(wb, "name=\"备注_说明_第二页\"") && !strstr(wb, "localSheetId=\"1\""), "sheet name sanitized, no filter on headerless sheet");
    free(wb); free(xlsx); free(js); office_report_free(&r3);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmp); if (system(cmd)) {}
}

/* ---------------- ⑦ PDF + PPTX ---------------- */
static bool has_bytes(const uint8_t *p, size_t n, const char *s) { const size_t k = strlen(s); for (size_t i = 0; i + k <= n; i++) if (!memcmp(p + i, s, k)) return true; return false; }
static void test_pdf_pptx(bool update) {
    char tmp[256]; snprintf(tmp, sizeof tmp, "/tmp/ds4_office_pdf_%ld", (long)getpid()); mkdir(tmp, 0755);
    char png[300]; snprintf(png, sizeof png, "%s/chart.png", tmp); make_png(png);
    size_t n; char *md = (char *)slurp(FIX "report.md", &n); office_node *d = office_md_parse(md);
    /* 非内嵌路: 产物与机器无关 → 金标 */
    office_opts ob = { tmp, NULL, NOW, "", "" }; office_report rb = {0}; size_t bn;
    uint8_t *pb = office_pdf_build(d, &ob, &rb, &bn);
    CHECK(pb && bn > 3000 && !memcmp(pb, "%PDF-1.4", 8) && has_bytes(pb, bn, "STSong-Light") && has_bytes(pb, bn, "startxref") && rb.notes.len && strstr(rb.notes.ptr, "not embedded"), "builtin-font pdf: %zu bytes, note=%s", bn, rb.notes.ptr ? rb.notes.ptr : "-");
    golden("report-builtin.pdf", pb, bn, update);
    free(pb); office_report_free(&rb);
    /* 内嵌路: 本机有 glyf 中文字体才跑; 子集比整字体小两个量级; ToUnicode 在; xref 偏移对得上 */
    const char *fp = office_ttf_find();
    if (!fp) printf("SKIP embedded-font pdf (no TrueType CJK font on this machine)\n");
    else {
        char err[256] = ""; office_ttf *f = office_ttf_load(fp, err, sizeof err);
        CHECK(f, "ttf load %s: %s", fp, err);
        if (f) {
            const int g = office_ttf_gid(f, 0x4E2D); CHECK(g > 0 && office_ttf_advance(f, g) > 500, "gid of 中 = %d adv %d", g, g ? office_ttf_advance(f, g) : 0);
            const int ga = office_ttf_gid(f, 'A');   /* 没拉丁字形的字体(DroidSansFallback)也合法: ASCII 走 Helvetica */
            CHECK(!ga || office_ttf_advance(f, ga) < 900, "A is proportional when present: gid %d adv %d", ga, ga ? office_ttf_advance(f, ga) : 0);
            int *map = malloc(sizeof(int) * (size_t)office_ttf_nglyphs(f)); for (int i = 0; i < office_ttf_nglyphs(f); i++) map[i] = -1;
            /* 挑一个 nhm 线后、左边距和第 nhm-1 项不同的字形(Droid: nhm 28492 < 49382 字形); 线后没有或都相同(Arial Unicode nhm = 字形数)就拿最后一个, 这条门在那台机器上只是走过场 */
            const int nhm = office_ttf_nhm(f); int last = office_ttf_nglyphs(f) - 1;
            for (int q = nhm; q < office_ttf_nglyphs(f); q++) if (office_ttf_lsb(f, q) != office_ttf_lsb(f, nhm - 1)) { last = q; break; }
            printf("  lsb gate: nhm %d of %d glyphs, probing glyph %d (lsb %d vs entry nhm-1 lsb %d)\n", nhm, office_ttf_nglyphs(f), last, office_ttf_lsb(f, last), office_ttf_lsb(f, nhm - 1));
            int nn = 2; map[0] = 0; map[g] = 1; if (ga) map[ga] = nn++; if (map[last] < 0) map[last] = nn++;   /* 新号必须连续: Droid 没有 A(ga=0), 照写 map[0]=2 会空出新号 0, 子集读到未初始化的下标(spark 段错误) */ size_t sl; uint8_t *sub = office_ttf_subset(f, map, &nn, &sl);
            CHECK(sub && sl < 200000 && nn >= 3 && !memcmp(sub, "\0\1\0\0", 4), "subset: %zu bytes, %d glyphs", sl, nn);
            /* 10-13 spark 实撞: 线后字形整对照抄第 nhm-1 项, 左边距错, "传"(亻+专 两个部件都在线后)渲染成两块叠在一起。子集的 hmtx 是满的, 第 k 项左边距必须等于原字体 */
            int lsb_bad = -1;
            if (sub) { const int nt = (sub[4] << 8) | sub[5]; for (int t = 0; t < nt; t++) if (!memcmp(sub + 12 + 16 * t, "hmtx", 4)) {
                const uint8_t *e = sub + 12 + 16 * t; const uint32_t ho = (uint32_t)e[8] << 24 | (uint32_t)e[9] << 16 | (uint32_t)e[10] << 8 | e[11]; lsb_bad = 0;
                for (int old = 0; old < office_ttf_nglyphs(f); old++) if (map[old] >= 0 && map[old] < nn) { const int got = (int16_t)(sub[ho + 4 * map[old] + 2] << 8 | sub[ho + 4 * map[old] + 3]); if (got != office_ttf_lsb(f, old)) lsb_bad++; } } }
            CHECK(lsb_bad == 0, "subset keeps every glyph's left side bearing (incl. glyph %d past numberOfHMetrics): %d wrong", last, lsb_bad);
            char sp[300]; snprintf(sp, sizeof sp, "%s/subset.ttf", tmp); put(sp, sub, sl);
            office_ttf *f2 = office_ttf_load(sp, err, sizeof err);   /* 子集自己能被我们的解析器读回(没 cmap 也能读 head/loca/glyf/hmtx) */
            CHECK(!f2, "subset has no cmap by design, so our loader refuses it: %s", err);
            office_ttf_free(f2); free(sub); free(map); office_ttf_free(f);
        }
        {   /* 主字体缺的符号走后备字体(/F4), 不落方块: Droid 没有 • → “” ① ✓, 列表圆点全是方块(10-13 spark 实撞)。Mac 的 Arial Unicode 自己全有, 不配后备 */
            char err[256] = ""; office_ttf *mf = office_ttf_load(fp, err, sizeof err); const char *sp = office_ttf_find_sym(fp); office_ttf *sf = sp ? office_ttf_load(sp, err, sizeof err) : NULL;
            const uint32_t syms[] = { 0x2022, 0x2192, 0x201C, 0x201D, 0x2014, 0x2026, 0x2460, 0x2713, 0xB1, 0xB0, 0 }; int miss = 0;
            for (int i = 0; syms[i]; i++) if (!(mf && office_ttf_gid(mf, syms[i])) && !(sf && office_ttf_gid(sf, syms[i]))) { miss++; printf("  no glyph for U+%04X in %s%s%s\n", syms[i], fp, sp ? " or " : "", sp ? sp : ""); }
            CHECK(miss == 0, "every common symbol is drawable by the main or the fallback font (fallback %s)", sp ? sp : "none");
            office_node *sd = office_md_parse("- 列表 → 下一步, “引号” ① ✓ 30°\n"); office_opts os = { tmp, NULL, NOW, NULL, NULL }; office_report rs = {0}; size_t sn;
            uint8_t *ps = office_pdf_build(sd, &os, &rs, &sn);
            CHECK(ps && (!sf || has_bytes(ps, sn, "/F4 ")) && (sf || !has_bytes(ps, sn, "/F4 ")), "symbols go to /F4 exactly when a fallback font is configured (%s)", sp ? sp : "none");
            free(ps); office_report_free(&rs); office_node_free(sd); office_ttf_free(sf); office_ttf_free(mf);
        }
        office_opts oe = { tmp, NULL, NOW, NULL, NULL }; office_report re = {0}; size_t en;
        uint8_t *pe = office_pdf_build(d, &oe, &re, &en);
        CHECK(pe && en > 10000 && en < 400000 && has_bytes(pe, en, "/FontFile2") && has_bytes(pe, en, "/ToUnicode") && has_bytes(pe, en, "/CIDToGIDMap /Identity") && !re.notes.len && re.images == 1, "embedded pdf: %zu bytes images %d notes %s", en, re.images, re.notes.ptr ? re.notes.ptr : "-");   /* 流都压过, 只能看字典键 */
        /* startxref 指向的位置真是 xref */
        const char *sx = NULL; for (size_t i = en; i-- > 9; ) if (!memcmp(pe + i, "startxref", 9)) { sx = (const char *)pe + i + 10; break; }
        const long off = sx ? atol(sx) : -1; CHECK(off > 0 && (size_t)off < en && !memcmp(pe + off, "xref", 4), "startxref → xref at %ld", off);
        char f1[300]; snprintf(f1, sizeof f1, "%s/report.pdf", tmp); put(f1, pe, en);
        if (have("mdimport")) {   /* macOS Spotlight 的 PDF 导入器: 能抽出文字 = 文本层与 ToUnicode 都对 */
            char cmd[1024]; snprintf(cmd, sizeof cmd, "/usr/bin/mdimport -t -d2 '%s' 2>/dev/null | grep -o 'Text content of [0-9]* characters'", f1);
            FILE *pf = popen(cmd, "r"); char line[128] = ""; if (pf) { if (!fgets(line, sizeof line, pf)) line[0] = 0; pclose(pf); }
            const int chars = line[0] ? atoi(line + 16) : 0; CHECK(chars > 200, "mdimport extracts text from our pdf: %s", line[0] ? line : "(nothing)");
        } else printf("SKIP mdimport text check (not macOS)\n");
        if (have("pdftotext")) {   /* poppler(spark 有): 抽回的正文含中文标题与英文词 = 两种字体的文本层都对 */
            char cmd[1024]; snprintf(cmd, sizeof cmd, "/usr/bin/pdftotext '%s' - 2>/dev/null", f1);
            FILE *pf = popen(cmd, "r"); ds4_buf tb = {0}; char buf[4096]; size_t k; while (pf && (k = fread(buf, 1, sizeof buf, pf)) > 0) ds4_buf_append(&tb, buf, k); if (pf) pclose(pf); ds4_buf_putc(&tb, 0);
            CHECK(tb.len > 100 && strstr(tb.ptr, "三季度经营分析") && strstr(tb.ptr, "GROUP BY region") && strstr(tb.ptr, "snake_case_word"), "pdftotext roundtrip (%zu bytes)", tb.len);
            ds4_buf_free(&tb);
        } else printf("SKIP pdftotext check (not installed)\n");
        free(pe); office_report_free(&re);
    }
    office_node_free(d); free(md);
    /* pptx: 夹具 slides.md → 6 页(标题页/要点+备注/表格/图+要点/有序+引用+代码/章节页) */
    char *sm = (char *)slurp(FIX "slides.md", &n); office_node *sd = office_md_parse(sm);
    office_opts op = { tmp, NULL, NOW, NULL, NULL }; office_report rp = {0}; size_t pn;
    uint8_t *px = office_pptx_build(sd, &op, &rp, &pn);
    CHECK(px && pn > 5000 && rp.images == 1 && !rp.missing_images, "pptx built %zu bytes images %d", pn, rp.images);
    golden("slides.pptx", px, pn, update);
    char f2[300]; snprintf(f2, sizeof f2, "%s/slides.pptx", tmp); put(f2, px, pn);
    external_checks(f2, tmp, NULL);
    char p1[400]; snprintf(p1, sizeof p1, "%s/x/ppt/presentation.xml", tmp); size_t wn; char *pres = (char *)slurp(p1, &wn);
    CHECK(pres && strstr(pres, "r:id=\"rId7\"") && !strstr(pres, "r:id=\"rId8\"/></p:sldIdLst>") && strstr(pres, "notesMasterIdLst"), "6 slides + notes master listed");
    snprintf(p1, sizeof p1, "%s/x/ppt/notesSlides/notesSlide2.xml", tmp); char *ns = (char *)slurp(p1, &wn);
    CHECK(ns && strstr(ns, "讲稿：强调不用装任何工具"), "slide 2 speaker notes");
    snprintf(p1, sizeof p1, "%s/x/ppt/slides/slide4.xml", tmp); char *s4 = (char *)slurp(p1, &wn);
    CHECK(s4 && strstr(s4, "<p:pic>") && strstr(s4, "r:embed=\"rId2\"") && strstr(s4, "营收连续两季增长"), "slide 4 has picture + bullets");
    snprintf(p1, sizeof p1, "%s/x/ppt/slides/slide3.xml", tmp); char *s3 = (char *)slurp(p1, &wn);
    CHECK(s3 && strstr(s3, "<a:tbl>") && strstr(s3, "<a:gridCol") && strstr(s3, "1,385"), "slide 3 has a table");
    free(pres); free(ns); free(s4); free(s3); free(px); office_report_free(&rp); office_node_free(sd); free(sm);
    char cmd[512]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmp); if (system(cmd)) {}
}

/* ---------------- ⑥ PNG 解码(含滤波行): 自造 3x2 RGB, 第二行 Up 滤波 ---------------- */
static void test_png(void) {
    uint8_t raw[2 * (1 + 9)] = { 0, 10, 20, 30, 40, 50, 60, 70, 80, 90,  2, 1, 1, 1, 1, 1, 1, 1, 1, 1 };   /* 行 0 原样; 行 1 = Up: 每字节 +1 */
    uint8_t *def; const size_t dn = office_deflate(raw, sizeof raw, &def);
    ds4_buf png = {0}; static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A }; ds4_buf_append(&png, sig, 8);
    uint8_t ihdr[13] = { 0, 0, 0, 3, 0, 0, 0, 2, 8, 2, 0, 0, 0 };
    uint8_t len[4], crc[4];
#define CH(type, data, n_) do { be32(len, (uint32_t)(n_)); ds4_buf_append(&png, len, 4); size_t at = png.len; ds4_buf_append(&png, type, 4); ds4_buf_append(&png, data, n_); be32(crc, office_crc32(png.ptr + at, (n_) + 4)); ds4_buf_append(&png, crc, 4); } while (0)
    CH("IHDR", ihdr, 13);
    uint8_t *idat = malloc(dn + 6); idat[0] = 0x78; idat[1] = 0x01; memcpy(idat + 2, def, dn); memset(idat + 2 + dn, 0, 4);
    CH("IDAT", idat, dn + 6); CH("IEND", idat, 0);
#undef CH
    int w, h; uint8_t *rgba;
    CHECK(office_png_decode((uint8_t *)png.ptr, png.len, &w, &h, &rgba) && w == 3 && h == 2, "png decode shape");
    CHECK(rgba && rgba[0] == 10 && rgba[5] == 50 && rgba[11] == 255, "row 0 raw: %d %d a=%d", rgba ? rgba[0] : -1, rgba ? rgba[5] : -1, rgba ? rgba[11] : -1);
    CHECK(rgba && rgba[12] == 11 && rgba[12 + 4 * 2 + 2] == 91, "row 1 Up-filtered: %d %d", rgba ? rgba[12] : -1, rgba ? rgba[22] : -1);
    free(rgba); free(idat); free(def); ds4_buf_free(&png);
}

int main(int argc, char **argv) {
    const bool update = argc > 1 && !strcmp(argv[1], "--update-golden");
    test_flate(); test_md(); test_calc(); test_build(update); test_png(); test_pdf_pptx(update);
    printf("office: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
