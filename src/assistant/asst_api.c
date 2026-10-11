/* asst_api.c — /api/assistant/… 路由(挂在 ds4-train 主进程上; ds4-server 直接开端口时没有助理)。
 * 鉴权(方案稿 §4.7): ds4-train 默认绑 0.0.0.0, 训练页对局域网本来就开着, 但助理有文件与命令工具, 不能这样:
 *   ① 对端不是本机 → 必须带 Authorization: Bearer <~/.ds4/assistant/token>;
 *   ② 对端是本机但 Host 头不是 localhost/127.0.0.1/[::1] → 拒(DNS 重绑定: 恶意网页让 evil.com 解析到 127.0.0.1, 浏览器带的 Host 是 evil.com);
 *   ③ 所有 POST 必须带 X-DS4-Assistant: 1 —— 你自己浏览器里开着的恶意网页能往 127.0.0.1:8000 发"简单 POST"(text/plain 不预检),
 *      带了自定义头的跨源请求浏览器必先预检, 我们不答 CORS, 它就发不出来。GET 只读、跨源读不到响应(同源策略), 不用这道头。
 * 出错会怎样: 401/403 带一句原因; 页面见 401 就弹口令框。 */
#include "asst_internal.h"
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

bool asst_api_path(const char *path) { return !strncmp(path, "/api/assistant/", 15); }

static bool eq_const(const char *a, const char *b) {   /* 定长比较: 不按第一个不同字节提前返回, 口令不能被逐字节计时猜出来 */
    const size_t n = strlen(a), m = strlen(b);
    unsigned char d = (unsigned char)(n != m);
    for (size_t i = 0; i < n && i < m; i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0 && n == m;
}

static bool local_host(const char *host) {
    char h[256]; snprintf(h, sizeof h, "%s", host);
    if (h[0] == '[') { char *e = strchr(h, ']'); if (e) e[1] = 0; } else { char *c = strchr(h, ':'); if (c) *c = 0; }
    return !strcasecmp(h, "localhost") || !strcmp(h, "127.0.0.1") || !strcmp(h, "[::1]");
}

bool asst_authorized(const asst_req *rq, int *code, const char **why) {
    const bool tok = asst_token[0] && !strncmp(rq->auth, "Bearer ", 7) && eq_const(rq->auth + 7, asst_token);
    if (!tok && !rq->peer_local) { *code = 401; *why = "token required: this request is not from this machine (the token is in ~/.ds4/assistant/token)"; return false; }
    if (!tok && !local_host(rq->host)) { *code = 403; *why = "Host header is not localhost (DNS rebinding?); open the page at http://127.0.0.1:<port>/ or use the token"; return false; }
    if (!strcmp(rq->method, "POST") && strcmp(rq->xhdr, "1")) { *code = 403; *why = "missing X-DS4-Assistant header"; return false; }
    return true;
}

static int jerr(ds4_buf *out, int code, const char *msg) { ds4_buf_puts(out, "{\"error\":"); ds4_json_escape(out, msg); ds4_buf_putc(out, '}'); return code; }

static void title_of(const char *text, char *out, size_t n) {   /* 首条消息前 ~40 字节, 不切断 UTF-8 */
    size_t k = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p && k + 4 < n && k < 40; ) {
        int len = *p < 0x80 ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : 4;
        if (*p == '\n' || *p == '\r') { out[k++] = ' '; p++; continue; }
        for (int i = 0; i < len && p[i]; i++) out[k++] = (char)p[i];
        p += len;
    }
    out[k] = 0;
}

static void audit_tail(ds4_buf *b, int max) {
    char p[ASST_PATH + 24]; snprintf(p, sizeof p, "%s/audit.jsonl", asst_root);
    size_t n = 0; char *all = asst_slurp(p, &n);
    ds4_buf_putc(b, '[');
    int k = 0;
    if (all) {   /* 倒着走: 最新的在前 */
        char *end = all + n;
        while (end > all && k < max) {
            if (end[-1] == '\n') { end--; continue; }
            char *s = end; while (s > all && s[-1] != '\n') s--;
            if (*s == '{') { ds4_buf_puts(b, k++ ? "," : ""); ds4_buf_append(b, s, (size_t)(end - s)); }
            end = s;
        }
    }
    free(all);
    ds4_buf_putc(b, ']');
}

int asst_api(const asst_req *rq, ds4_buf *out, const char **ctype) {
    *ctype = "application/json; charset=utf-8";
    if (!asst_root[0]) return jerr(out, 503, "assistant storage is not initialised (see ds4-train log)");
    const char *r = rq->path + 15;
    const bool post = !strcmp(rq->method, "POST");
    char err[512] = "";
    if (!strcmp(r, "status")) {
        ds4_buf_printf(out, "{\"sandbox\":\"%s\",\"browser\":\"%s\",\"workspace\":", asst_sbx_kind(), asst_browser_kind());
        ds4_json_escape(out, asst_ws); ds4_buf_puts(out, ",\"root\":"); ds4_json_escape(out, asst_root);
        ds4_buf_printf(out, ",\"local\":%s}", rq->peer_local ? "true" : "false");
        return 200;
    }
    if (!strcmp(r, "sessions")) { asst_sessions_json(out); return 200; }
    if (!strcmp(r, "memory")) {
        if (post) { if (!asst_memory_save(rq->body, err, sizeof err)) return jerr(out, 400, err); }
        asst_memory_json(out); return 200;
    }
    if (!strcmp(r, "activity")) {
        ds4_buf_puts(out, "{\"audit\":"); audit_tail(out, 300);
        ds4_buf_puts(out, ",\"approvals\":"); asst_approvals_json(out);
        char *pj = asst_policy_json(); ds4_buf_printf(out, ",\"policy\":%s}", pj); free(pj);
        return 200;
    }
    if (!post) return jerr(out, 405, "use POST");
    if (!strcmp(r, "policy")) { if (!asst_policy_op(rq->body, err, sizeof err)) return jerr(out, 400, err); ds4_buf_puts(out, "{\"ok\":true}"); return 200; }
    char *sid = NULL; asst_jstr(rq->body, "session", &sid);
    int code = 200;
    if (!strcmp(r, "send")) {
        char *text = NULL; asst_jstr(rq->body, "text", &text);
        char id[ASST_ID_LEN + 1];
        if (!text || !text[0]) code = jerr(out, 400, "empty message");
        else if (sid && sid[0] && !asst_id_ok(sid)) code = jerr(out, 400, "bad session id");
        else {
            if (sid && sid[0]) snprintf(id, sizeof id, "%s", sid);
            else { char t[64]; title_of(text, t, sizeof t); if (!asst_session_new(id, t, text)) { free(text); free(sid); return jerr(out, 500, "cannot create session"); } }
            if (!asst_run_submit(id, text, err, sizeof err)) code = jerr(out, 409, err);
            else ds4_buf_printf(out, "{\"session\":\"%s\"}", id);
        }
        free(text);
    } else if (!asst_id_ok(sid)) code = jerr(out, 400, "bad or missing session id");
    else if (!strcmp(r, "stop")) { asst_run_stop(sid); ds4_buf_puts(out, "{\"ok\":true}"); }
    else if (!strcmp(r, "approve")) {
        char *id = NULL, *dec = NULL; asst_jstr(rq->body, "id", &id); asst_jstr(rq->body, "decision", &dec);
        if (!asst_run_answer(sid, id ? id : "", dec ? dec : "", err, sizeof err)) code = jerr(out, 409, err); else ds4_buf_puts(out, "{\"ok\":true}");
        free(id); free(dec);
    } else if (!strcmp(r, "delete")) {
        /* 删会话 = 挪进 trash/sessions/(不真删: 点错了还能从盘上拿回来) */
        asst_run_stop(sid);
        char from[ASST_PATH + 64], to[ASST_PATH + 64], d[ASST_PATH + 32];
        snprintf(d, sizeof d, "%s/trash", asst_root); mkdir(d, 0700);
        snprintf(d, sizeof d, "%s/trash/sessions", asst_root); mkdir(d, 0700);
        snprintf(from, sizeof from, "%s/sessions/%s", asst_root, sid); snprintf(to, sizeof to, "%s/trash/sessions/%s-%ld", asst_root, sid, (long)time(NULL));
        if (rename(from, to)) code = jerr(out, 404, "no such session"); else ds4_buf_puts(out, "{\"ok\":true}");
    } else code = jerr(out, 404, "unknown assistant route");
    free(sid);
    return code;
}
