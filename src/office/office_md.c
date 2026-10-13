/* office_md.c — Markdown 子集 → 文档树。一份解析器喂三个写出器(docx / pdf / pptx), 格式差异全在写出器。
 * 认的东西就是工具描述里承诺的: # ## ### 标题、段落、- 与 1. 列表(两级)、**粗** *斜* `码`、围栏代码、> 引用、| 表格 |、链接、图片、--- 分隔、<!-- 注释 -->。
 * 不是 CommonMark 全集: 没有 setext 标题、HTML 块、脚注、嵌套引用。模型写出不认的语法时按普通段落处理, 不丢字 —— 丢字比排错版严重。
 * 行内解析是单遍状态机: 样式开关(粗/斜/码)变了就把攒着的字符落成一个 TEXT 节点。 */
#include "ds4_office.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *s; size_t n; } mline;

static office_node *node(int kind) { office_node *n = calloc(1, sizeof *n); n->kind = kind; return n; }
static void add(office_node *parent, office_node *c) { if (!parent->child) { parent->child = c; return; } office_node *t = parent->child; while (t->next) t = t->next; t->next = c; }
static char *dupn(const char *s, size_t n) { char *d = malloc(n + 1); memcpy(d, s, n); d[n] = 0; return d; }

void office_node_free(office_node *n) {
    while (n) { office_node *nx = n->next; office_node_free(n->child); free(n->text); free(n->url); free(n); n = nx; }
}
static int count_list(const office_node *n, int kind) { int c = 0; for (; n; n = n->next) c += (n->kind == kind) + count_list(n->child, kind); return c; }
int office_count(const office_node *n, int kind) { return n ? (n->kind == kind) + count_list(n->child, kind) : 0; }   /* 只数 n 和它的子树, 不数兄弟 */

/* ---------------- 行内 ---------------- */
typedef struct { office_node *parent; ds4_buf run; bool bold, italic, code; } inl;
static void flush(inl *x) {
    if (!x->run.len) return;
    office_node *t = node(OFF_TEXT); t->text = ds4_buf_take(&x->run); t->bold = x->bold; t->italic = x->italic; t->code = x->code;
    add(x->parent, t);
}
/* [text](url) / ![alt](src): p 指向 '[', 成功返回 ')' 之后的位置 */
static const char *bracket(const char *p, char **text, char **url) {
    const char *q = p + 1; int depth = 1;
    while (*q && depth) { if (*q == '[') depth++; else if (*q == ']') depth--; if (depth) q++; }
    if (*q != ']' || q[1] != '(') return NULL;
    const char *u = q + 2, *e = u; int pd = 1;
    while (*e && pd) { if (*e == '(') pd++; else if (*e == ')') pd--; if (pd) e++; }
    if (*e != ')') return NULL;
    size_t ul = (size_t)(e - u); while (ul && u[ul - 1] == ' ') ul--;
    const char *sp = memchr(u, ' ', ul); if (sp) ul = (size_t)(sp - u);   /* [t](url "title"): 丢标题 */
    *text = dupn(p + 1, (size_t)(q - p - 1)); *url = dupn(u, ul);
    return e + 1;
}
static bool word_edge(const char *s, const char *p) {   /* _ 只在词边界算斜体开关: snake_case 里的下划线不是 */
    const bool before = p == s || isspace((unsigned char)p[-1]) || ispunct((unsigned char)p[-1]);
    const bool after = !p[1] || isspace((unsigned char)p[1]) || ispunct((unsigned char)p[1]);
    return before || after;
}
static void parse_inline(office_node *parent, const char *s) {
    inl x = { parent, {0}, false, false, false };
    for (const char *p = s; *p; ) {
        const char c = *p;
        if (c == '\\' && p[1]) { ds4_buf_putc(&x.run, p[1]); p += 2; continue; }
        if (c == '\n') { ds4_buf_putc(&x.run, ' '); p++; continue; }   /* 段内换行 = 软换行 */
        if (c == '`') {
            size_t ticks = 0; while (p[ticks] == '`') ticks++;
            const char *q = p + ticks, *e = NULL;
            for (; *q; q++) if (*q == '`') { size_t k = 0; while (q[k] == '`') k++; if (k == ticks) { e = q; break; } q += k - 1; }
            if (!e) { ds4_buf_append(&x.run, p, ticks); p += ticks; continue; }
            flush(&x); x.code = true;
            const char *a = p + ticks; size_t n = (size_t)(e - a);
            if (n > 1 && a[0] == ' ' && a[n - 1] == ' ') { a++; n -= 2; }
            ds4_buf_append(&x.run, a, n); flush(&x); x.code = false;
            p = e + ticks; continue;
        }
        if ((c == '*' || c == '_') && p[1] == c) { flush(&x); x.bold = !x.bold; p += 2; continue; }
        if (c == '*' || (c == '_' && word_edge(s, p))) { flush(&x); x.italic = !x.italic; p++; continue; }
        if (c == '!' && p[1] == '[') {
            char *t, *u; const char *e = bracket(p + 1, &t, &u);
            if (e) { flush(&x); office_node *im = node(OFF_IMAGE); im->text = t; im->url = u; add(parent, im); p = e; continue; }
        }
        if (c == '[') {
            char *t, *u; const char *e = bracket(p, &t, &u);
            if (e) { flush(&x); office_node *l = node(OFF_LINK); l->text = t; l->url = u; l->bold = x.bold; l->italic = x.italic; add(parent, l); p = e; continue; }
        }
        ds4_buf_putc(&x.run, c); p++;
    }
    flush(&x);
}

/* ---------------- 行分类 ---------------- */
static size_t indent_of(mline l) { size_t i = 0; while (i < l.n && l.s[i] == ' ') i++; return i; }
static bool blank(mline l) { for (size_t i = 0; i < l.n; i++) if (!isspace((unsigned char)l.s[i])) return false; return true; }
static bool starts(mline l, const char *pfx) { const size_t k = strlen(pfx); return l.n >= k && !memcmp(l.s, pfx, k); }
static bool is_fence(mline l) { const size_t i = indent_of(l); return i <= 3 && (starts((mline){ l.s + i, l.n - i }, "```") || starts((mline){ l.s + i, l.n - i }, "~~~")); }
static int heading_level(mline l) {
    size_t i = indent_of(l); if (i > 3) return 0;
    int k = 0; while (i + (size_t)k < l.n && l.s[i + k] == '#') k++;
    if (!k || k > 6) return 0;
    if (i + (size_t)k < l.n && l.s[i + k] != ' ') return 0;
    return k;
}
static bool is_hr(mline l) {
    size_t i = indent_of(l); if (i > 3 || i >= l.n) return false;
    const char c = l.s[i]; if (c != '-' && c != '*' && c != '_') return false;
    int cnt = 0;
    for (; i < l.n; i++) { if (l.s[i] == c) cnt++; else if (l.s[i] != ' ') return false; }
    return cnt >= 3;
}
/* 列表项: 返回标记后内容的偏移, 0 = 不是 */
static size_t list_marker(mline l, bool *ordered) {
    size_t i = indent_of(l);
    if (i < l.n && (l.s[i] == '-' || l.s[i] == '*' || l.s[i] == '+') && i + 1 < l.n && l.s[i + 1] == ' ') { *ordered = false; return i + 2; }
    size_t d = i; while (d < l.n && isdigit((unsigned char)l.s[d]) && d - i < 9) d++;
    if (d > i && d + 1 < l.n && (l.s[d] == '.' || l.s[d] == ')') && l.s[d + 1] == ' ') { *ordered = true; return d + 2; }
    return 0;
}
static bool is_table_sep(mline l) {
    bool dash = false;
    for (size_t i = 0; i < l.n; i++) { const char c = l.s[i]; if (c == '-') dash = true; else if (c != '|' && c != ':' && c != ' ') return false; }
    return dash && memchr(l.s, '|', l.n);
}
static bool is_image_line(mline l) {
    size_t i = indent_of(l); if (i + 1 >= l.n || l.s[i] != '!' || l.s[i + 1] != '[') return false;
    size_t e = l.n; while (e > i && isspace((unsigned char)l.s[e - 1])) e--;
    return e > i && l.s[e - 1] == ')';
}
static bool block_start(mline l, const mline *next) {
    bool o;
    return heading_level(l) || is_fence(l) || is_hr(l) || list_marker(l, &o) || starts((mline){ l.s + indent_of(l), l.n - indent_of(l) }, ">") ||
           starts((mline){ l.s + indent_of(l), l.n - indent_of(l) }, "<!--") || is_image_line(l) || (next && memchr(l.s, '|', l.n) && is_table_sep(*next));
}

/* ---------------- 块 ---------------- */
typedef struct { mline *l; int n, i; } cur;

static void split_cells(mline l, char **cells, int *nc, int max) {
    *nc = 0; size_t a = 0, e = l.n;
    while (a < e && l.s[a] == ' ') a++;
    while (e > a && (l.s[e - 1] == ' ' || l.s[e - 1] == '\r')) e--;
    if (a < e && l.s[a] == '|') a++;
    if (e > a && l.s[e - 1] == '|' && !(e >= 2 && l.s[e - 2] == '\\')) e--;
    ds4_buf b = {0};
    for (size_t i = a; i <= e; i++) {
        if (i == e || (l.s[i] == '|' && !(i > a && l.s[i - 1] == '\\'))) {
            char *c = ds4_buf_take(&b); size_t k = strlen(c), s = 0;
            while (k && c[k - 1] == ' ') c[--k] = 0;
            while (c[s] == ' ') s++;
            if (s) memmove(c, c + s, k - s + 1);
            if (*nc < max) cells[(*nc)++] = c; else free(c);
            b = (ds4_buf){0};
        } else if (l.s[i] == '\\' && i + 1 < e && l.s[i + 1] == '|') { ds4_buf_putc(&b, '|'); i++; }
        else ds4_buf_putc(&b, l.s[i]);
    }
    ds4_buf_free(&b);
}
static void table(cur *c, office_node *doc) {
    office_node *t = node(OFF_TABLE); add(doc, t);
    char *cells[64]; int nc;
    split_cells(c->l[c->i], cells, &nc, 64); t->ncols = nc;
    office_node *hr = node(OFF_ROW); hr->header = true; add(t, hr);
    for (int k = 0; k < nc; k++) { office_node *cell = node(OFF_CELL); add(hr, cell); parse_inline(cell, cells[k]); free(cells[k]); }
    c->i += 2;
    while (c->i < c->n && !blank(c->l[c->i]) && memchr(c->l[c->i].s, '|', c->l[c->i].n)) {
        split_cells(c->l[c->i], cells, &nc, 64);
        office_node *row = node(OFF_ROW); add(t, row);
        for (int k = 0; k < t->ncols; k++) { office_node *cell = node(OFF_CELL); add(row, cell); if (k < nc) parse_inline(cell, cells[k]); }
        for (int k = 0; k < nc; k++) free(cells[k]);
        c->i++;
    }
}
static void list(cur *c, office_node *parent, size_t base, int level) {
    bool ordered; list_marker(c->l[c->i], &ordered);
    office_node *ls = node(OFF_LIST); ls->ordered = ordered; ls->level = level; add(parent, ls);
    office_node *item = NULL; ds4_buf text = {0};
    while (c->i < c->n) {
        mline l = c->l[c->i];
        if (blank(l)) {   /* 空行后还是本列表的项才继续(松散列表) */
            int j = c->i + 1; while (j < c->n && blank(c->l[j])) j++;
            bool o2; if (j < c->n && list_marker(c->l[j], &o2) && indent_of(c->l[j]) >= base) { c->i = j; continue; }
            break;
        }
        bool o2; const size_t off = list_marker(l, &o2); const size_t ind = indent_of(l);
        if (off && ind >= base + 2 && item && level < 2) { if (text.len) { parse_inline(item, text.ptr); ds4_buf_free(&text); text = (ds4_buf){0}; } list(c, item, ind, level + 1); continue; }
        if (off && ind >= base && (ind < base + 2 || level >= 2)) {
            if (item && text.len) { parse_inline(item, text.ptr); ds4_buf_free(&text); text = (ds4_buf){0}; }
            item = node(OFF_ITEM); add(ls, item);
            ds4_buf_append(&text, l.s + off, l.n - off);
            c->i++; continue;
        }
        if (off && ind < base) break;
        if (item && !off && ind > base && !block_start(l, NULL)) { ds4_buf_putc(&text, '\n'); ds4_buf_append(&text, l.s + ind, l.n - ind); c->i++; continue; }   /* 续行 */
        break;
    }
    if (item && text.len) parse_inline(item, text.ptr);
    ds4_buf_free(&text);
}

office_node *office_md_parse(const char *md) {
    int cap = 64, n = 0; mline *ls = malloc(sizeof *ls * (size_t)cap);
    for (const char *p = md; *p; ) {
        const char *nl = strchr(p, '\n'); size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len && p[len - 1] == '\r') len--;
        if (n == cap) { cap *= 2; ls = realloc(ls, sizeof *ls * (size_t)cap); }
        ls[n++] = (mline){ p, len };
        p = nl ? nl + 1 : p + len;
    }
    office_node *doc = node(OFF_DOC); cur c = { ls, n, 0 };
    ds4_buf para = {0};
    while (c.i < n) {
        mline l = ls[c.i]; const mline *next = c.i + 1 < n ? &ls[c.i + 1] : NULL;
        if (blank(l)) { c.i++; continue; }
        int hl = heading_level(l);
        if (hl) {
            office_node *h = node(OFF_HEADING); h->level = hl; add(doc, h);
            size_t a = indent_of(l) + (size_t)hl, e = l.n; while (a < e && l.s[a] == ' ') a++;
            while (e > a && (l.s[e - 1] == ' ' || l.s[e - 1] == '#')) e--;
            char *t = dupn(l.s + a, e - a); parse_inline(h, t); free(t); c.i++; continue;
        }
        if (is_fence(l)) {
            const size_t ind = indent_of(l); const char fc = l.s[ind];
            office_node *cd = node(OFF_CODE); add(doc, cd);
            for (c.i++; c.i < n; c.i++) {
                mline q = ls[c.i]; const size_t qi = indent_of(q);
                if (qi <= 3 && qi < q.n && q.s[qi] == fc && q.n - qi >= 3 && q.s[qi + 1] == fc && q.s[qi + 2] == fc) { c.i++; break; }
                ds4_buf_append(&para, q.s, q.n); ds4_buf_putc(&para, '\n');
            }
            cd->text = ds4_buf_take(&para); para = (ds4_buf){0}; continue;
        }
        if (is_hr(l)) { add(doc, node(OFF_HR)); c.i++; continue; }
        if (starts((mline){ l.s + indent_of(l), l.n - indent_of(l) }, "<!--")) {
            office_node *nt = node(OFF_NOTES); add(doc, nt);
            const char *start = l.s + indent_of(l) + 4, *end = NULL;
            for (; c.i < n; c.i++) { mline q = ls[c.i]; const char *e = NULL; for (size_t k = 0; k + 3 <= q.n; k++) if (!memcmp(q.s + k, "-->", 3)) { e = q.s + k; break; } if (e && e >= start) { end = e; c.i++; break; } }
            if (!end) end = md + strlen(md);
            size_t k = (size_t)(end - start); while (k && isspace((unsigned char)start[k - 1])) k--; while (k && isspace((unsigned char)*start)) { start++; k--; }
            nt->text = dupn(start, k); continue;
        }
        if (next && memchr(l.s, '|', l.n) && is_table_sep(*next)) { table(&c, doc); continue; }
        if (l.s[indent_of(l)] == '>') {
            office_node *q = node(OFF_QUOTE); add(doc, q);
            while (c.i < n && !blank(ls[c.i]) && ls[c.i].s[indent_of(ls[c.i])] == '>') {
                mline m = ls[c.i]; size_t a = indent_of(m) + 1; if (a < m.n && m.s[a] == ' ') a++;
                if (para.len) ds4_buf_putc(&para, '\n');
                ds4_buf_append(&para, m.s + a, m.n - a); c.i++;
            }
            parse_inline(q, para.ptr); ds4_buf_free(&para); para = (ds4_buf){0}; continue;
        }
        bool ordered;
        if (list_marker(l, &ordered)) { list(&c, doc, indent_of(l), 1); continue; }
        if (is_image_line(l)) {
            char *t, *u; const size_t i0 = indent_of(l); char *line = dupn(l.s + i0, l.n - i0);
            if (bracket(line + 1, &t, &u)) { office_node *im = node(OFF_IMAGE); im->text = t; im->url = u; add(doc, im); free(line); c.i++; continue; }
            free(line);
        }
        /* 段落: 收到空行或下一个块的开头 */
        office_node *p = node(OFF_PARA); add(doc, p);
        while (c.i < n && !blank(ls[c.i]) && (!para.len || !block_start(ls[c.i], c.i + 1 < n ? &ls[c.i + 1] : NULL))) {
            mline m = ls[c.i]; const size_t a = indent_of(m);
            if (para.len) ds4_buf_putc(&para, '\n');
            ds4_buf_append(&para, m.s + a, m.n - a); c.i++;
        }
        parse_inline(p, para.ptr ? para.ptr : ""); ds4_buf_free(&para); para = (ds4_buf){0};
    }
    ds4_buf_free(&para); free(ls);
    return doc;
}
