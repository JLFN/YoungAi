/* asst_json.c — 在一个 JSON 对象的顶层按 key 取值。词法全走 src/common/ds4_json(一件事一处), 这里只是"找到这个 key"的那层循环。
 * 只看顶层: 工具参数 {"path":"a","content":"…"} 与页面请求体都是扁平的; 嵌套里同名的 key 不会被误取(跳过是整值跳)。
 * 出错会怎样: 不是对象 / 中途坏掉 → 当"没有这个 key", 调用方按缺参数报给模型, 模型下一轮自己改。 */
#include "asst_internal.h"

static bool find_key(const char *obj, const char *key, const char **val) {
    if (!obj) return false;
    const char *p = obj;
    json_ws(&p);
    if (*p != '{') return false;
    p++;
    for (;;) {
        json_ws(&p);
        if (*p == '}') return false;
        char *k = NULL;
        if (!json_string(&p, &k)) return false;
        json_ws(&p);
        if (*p != ':') { free(k); return false; }
        p++;
        json_ws(&p);
        const bool hit = !strcmp(k, key);
        free(k);
        if (hit) { *val = p; return true; }
        if (!json_skip_value(&p)) return false;
        json_ws(&p);
        if (*p == ',') { p++; continue; }
        return false;
    }
}

bool asst_jstr(const char *obj, const char *key, char **out) {
    const char *p;
    *out = NULL;
    if (!find_key(obj, key, &p) || *p != '"') return false;
    return json_string(&p, out);
}

bool asst_jint(const char *obj, const char *key, long *out) {
    const char *p; double d;
    if (!find_key(obj, key, &p)) return false;
    if (*p == '"') {   /* 模型常把数字写成字符串 "40": 照收, 不然一个引号就让工具报错一轮 */
        char *s = NULL;
        if (!json_string(&p, &s)) return false;
        char *end; const long v = strtol(s, &end, 10);
        const bool ok = end != s && *end == 0;
        free(s);
        if (ok) *out = v;
        return ok;
    }
    if (!json_number(&p, &d)) return false;
    *out = (long)d;
    return true;
}

bool asst_jbool(const char *obj, const char *key, bool *out) {
    const char *p;
    if (!find_key(obj, key, &p)) return false;
    if (*p == '"') { char *s = NULL; if (!json_string(&p, &s)) return false; *out = !strcmp(s, "true"); free(s); return true; }
    return json_bool(&p, out);
}

bool asst_jarr_at(const char *arr, int i, char **out) {
    *out = NULL;
    if (!arr) return false;
    const char *p = arr;
    json_ws(&p);
    if (*p != '[') return false;
    p++;
    for (int k = 0;; k++) {
        json_ws(&p);
        if (*p == ']') return false;
        if (k == i) return json_raw_value(&p, out);
        if (!json_skip_value(&p)) return false;
        json_ws(&p);
        if (*p == ',') { p++; continue; }
        return false;
    }
}

bool asst_jraw(const char *obj, const char *key, char **out) {
    const char *p;
    *out = NULL;
    if (!find_key(obj, key, &p)) return false;
    return json_raw_value(&p, out);
}
