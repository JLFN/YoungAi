/* office_calc.c — xlsx 公式求值, 只为填 <v> 缓存值(方案稿 §3.5): iOS 微信预览不算公式, 只显示文件里的缓存值; 不填, 同事点开看到的"合计"是空格。
 * 认: SUM / AVERAGE / MIN / MAX / COUNT, + - * / ^, 一元负号, 括号, A1 引用, A1:B9 区间。
 * 不认的(字符串、比较、& 拼接、别的函数)不猜: 整格标 state 4, xlsx 只写 <f> 不写 <v>, Excel 打开(fullCalcOnLoad)自己算; 结果文本里列出来。
 * Excel 语义照抄: 算术里碰到文本 → #VALUE!; 聚合函数里的文本格跳过; 除零 → #DIV/0!; 引用越界或成环 → #REF!。
 * 引用到的格若也是公式就先递归算它; 算中(state 1)再被引用 = 环。 */
#include "office_internal.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>

typedef struct { office_sheet *s; const char *p; bool unsup; const char *err; } ev;
typedef struct { double v; bool is_err; const char *err; } val;   /* 中间值: 数或错误 */
static val vnum(double d) { val v = { d, false, NULL }; return v; }
static val verr(const char *e) { val v = { 0, true, e }; return v; }

office_cell *office_cell_at(office_sheet *s, int r, int c) { return r >= 0 && c >= 0 && r < s->rows && c < s->cols ? &s->cells[r * s->cols + c] : NULL; }
void office_sheet_free(office_sheet *s) {
    if (!s) return;
    for (int i = 0; i < s->rows * s->cols; i++) free(s->cells[i].str);
    free(s->cells); free(s->name); memset(s, 0, sizeof *s);
}
void office_calc_colname(int c, char out[8]) {
    char tmp[8]; int k = 0;
    do { tmp[k++] = (char)('A' + c % 26); c = c / 26 - 1; } while (c >= 0 && k < 7);
    for (int i = 0; i < k; i++) out[i] = tmp[k - 1 - i];
    out[k] = 0;
}
bool office_calc_ref(const char *ref, int *r, int *c, const char **end) {
    const char *p = ref; int col = 0, nl = 0;
    if (*p == '$') p++;
    while (isalpha((unsigned char)*p) && nl < 3) { col = col * 26 + (toupper((unsigned char)*p) - 'A' + 1); p++; nl++; }
    if (!nl) return false;
    if (*p == '$') p++;
    if (!isdigit((unsigned char)*p)) return false;
    long row = 0; int nd = 0; while (isdigit((unsigned char)*p) && nd < 7) { row = row * 10 + (*p - '0'); p++; nd++; }
    if (isalnum((unsigned char)*p) || row < 1) return false;   /* "A1B" 这种不是引用 */
    *r = (int)row - 1; *c = col - 1; *end = p;
    return true;
}

static void ws(ev *e) { while (*e->p == ' ') e->p++; }
static val expr(ev *e);

/* 一格的值给算术用: 空=0, 数, 文本能转数就转否则 #VALUE!, 公式递归 */
static val cell_value(ev *e, int r, int c, bool in_agg, bool *skip) {
    *skip = false;
    office_cell *x = office_cell_at(e->s, r, c);
    if (!x) return verr("#REF!");
    switch (x->type) {
    case CELL_EMPTY: if (in_agg) *skip = true; return vnum(0);
    case CELL_NUM: return vnum(x->num);
    case CELL_STR: {
        if (in_agg) { *skip = true; return vnum(0); }
        char *end; const double d = strtod(x->str, &end);
        return (end != x->str && !*end) ? vnum(d) : verr("#VALUE!");
    }
    default:
        if (x->state == 1) return verr("#REF!");   /* 环 */
        if (x->state == 0) office_calc_eval(e->s, r, c);
        if (x->state == 4) { e->unsup = true; return vnum(0); }
        if (x->state == 3) return verr(x->errv);
        return vnum(x->num);
    }
}

typedef struct { double sum, min, max; long n; val err; } agg;
static void agg_add(agg *a, val v) {
    if (v.is_err) { if (!a->err.is_err) a->err = v; return; }
    if (!a->n || v.v < a->min) a->min = v.v;
    if (!a->n || v.v > a->max) a->max = v.v;
    a->sum += v.v; a->n++;
}
/* 聚合函数的一个参数: 区间或表达式 */
static void arg(ev *e, agg *a) {
    ws(e);
    int r1, c1, r2, c2; const char *q;
    if (office_calc_ref(e->p, &r1, &c1, &q) && *q == ':' && office_calc_ref(q + 1, &r2, &c2, &q)) {
        e->p = q;
        if (r2 < r1) { int t = r1; r1 = r2; r2 = t; }
        if (c2 < c1) { int t = c1; c1 = c2; c2 = t; }
        for (int r = r1; r <= r2; r++) for (int c = c1; c <= c2; c++) { bool skip; val v = cell_value(e, r, c, true, &skip); if (!skip) agg_add(a, v); }
        return;
    }
    agg_add(a, expr(e));
}
static val func(ev *e, const char *name) {
    agg a = { 0, 0, 0, 0, vnum(0) };
    ws(e); if (*e->p != '(') { e->unsup = true; return vnum(0); }
    e->p++;
    ws(e);
    if (*e->p != ')') for (;;) { arg(e, &a); ws(e); if (*e->p == ',' || *e->p == ';') { e->p++; continue; } break; }
    if (*e->p != ')') { e->unsup = true; return vnum(0); }
    e->p++;
    if (a.err.is_err) return a.err;
    if (!strcmp(name, "SUM")) return vnum(a.sum);
    if (!strcmp(name, "COUNT")) return vnum((double)a.n);
    if (!strcmp(name, "MIN")) return vnum(a.n ? a.min : 0);
    if (!strcmp(name, "MAX")) return vnum(a.n ? a.max : 0);
    if (!strcmp(name, "AVERAGE")) return a.n ? vnum(a.sum / (double)a.n) : verr("#DIV/0!");
    e->unsup = true; return vnum(0);
}

static val factor(ev *e) {
    ws(e);
    if (*e->p == '-') { e->p++; val v = factor(e); if (!v.is_err) v.v = -v.v; return v; }
    if (*e->p == '+') { e->p++; return factor(e); }
    if (*e->p == '(') { e->p++; val v = expr(e); ws(e); if (*e->p == ')') e->p++; else e->unsup = true; return v; }
    if (isdigit((unsigned char)*e->p) || (*e->p == '.' && isdigit((unsigned char)e->p[1]))) {
        char *end; const double d = strtod(e->p, &end); e->p = end;
        if (*e->p == '%') { e->p++; return vnum(d / 100); }
        return vnum(d);
    }
    int r, c; const char *q;
    if (office_calc_ref(e->p, &r, &c, &q)) {
        if (*q == ':') { e->unsup = true; e->p = q; return vnum(0); }   /* 裸区间不在函数里: Excel 也不让 */
        e->p = q; bool skip; return cell_value(e, r, c, false, &skip);
    }
    if (isalpha((unsigned char)*e->p)) {
        char name[16]; int k = 0;
        while ((isalnum((unsigned char)*e->p) || *e->p == '.' || *e->p == '_') && k < 15) name[k++] = (char)toupper((unsigned char)*e->p++);
        while (isalnum((unsigned char)*e->p) || *e->p == '.' || *e->p == '_') e->p++;
        name[k] = 0;
        return func(e, name);
    }
    e->unsup = true;   /* 字符串、比较、& 拼接、别的东西 */
    return vnum(0);
}
static val power(ev *e) {
    val a = factor(e); ws(e);
    while (*e->p == '^') {
        e->p++; val b = factor(e);
        if (a.is_err) return a;
        if (b.is_err) return b;
        if (a.v < 0 && b.v != floor(b.v)) return verr("#NUM!");   /* 负数开分数次方 = NaN; 全仓 -ffast-math 下 isnan 会被折掉, 所以在算之前就判 */
        a.v = pow(a.v, b.v);
        if (fabs(a.v) > 1e300) return verr("#NUM!");
        ws(e);
    }
    return a;
}
static val term(ev *e) {
    val a = power(e); ws(e);
    while (*e->p == '*' || *e->p == '/') {
        const char op = *e->p++; val b = power(e);
        if (a.is_err) return a;
        if (b.is_err) return b;
        if (op == '*') a.v *= b.v; else if (b.v == 0) return verr("#DIV/0!"); else a.v /= b.v;
        ws(e);
    }
    return a;
}
static val expr(ev *e) {
    val a = term(e); ws(e);
    while (*e->p == '+' || *e->p == '-') {
        const char op = *e->p++; val b = term(e);
        if (a.is_err) return a;
        if (b.is_err) return b;
        a.v = op == '+' ? a.v + b.v : a.v - b.v;
        ws(e);
    }
    return a;
}

void office_calc_eval(office_sheet *s, int r, int c) {
    office_cell *x = office_cell_at(s, r, c);
    if (!x || x->type != CELL_FORMULA || x->state) return;
    x->state = 1;
    ev e = { s, x->str ? x->str : "", false, NULL };
    val v = expr(&e); ws(&e);
    if (*e.p) e.unsup = true;   /* 没吃完 = 有不认识的语法 */
    if (e.unsup) { x->state = 4; return; }
    if (v.is_err) { x->state = 3; x->errv = v.err; return; }
    if (fabs(v.v) > 1e300) { x->state = 3; x->errv = "#NUM!"; return; }
    x->state = 2; x->num = v.v;
}
