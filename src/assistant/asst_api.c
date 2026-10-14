/* asst_api.c — /api/assistant/… 路由(挂在 ds4-train 主进程上; ds4-server 直接开端口时没有助理)。
 * 鉴权(方案稿 §4.7): ds4-train 默认绑 0.0.0.0, 训练页对局域网本来就开着, 但助理有文件与命令工具, 不能这样:
 *   ① 对端不是本机 → 必须带 Authorization: Bearer <~/.ds4/assistant/token>;
 *   ② 对端是本机但 Host 头不是 localhost/127.0.0.1/[::1] → 拒(DNS 重绑定: 恶意网页让 evil.com 解析到 127.0.0.1, 浏览器带的 Host 是 evil.com);
 *   ③ 所有 POST 必须带 X-DS4-Assistant: 1 —— 你自己浏览器里开着的恶意网页能往 127.0.0.1:8000 发"简单 POST"(text/plain 不预检),
 *      带了自定义头的跨源请求浏览器必先预检, 我们不答 CORS, 它就发不出来。GET 只读、跨源读不到响应(同源策略), 不用这道头。
 * 10-12 起工作台有登录(src/studio/studio_auth.c), 闸在主进程最前面, 走到这里的请求都已登录或带本机口令; 登录票(signed_in)在这里等同口令,
 *   所以从别的设备登录后不用再输一遍口令。①② 留着当第二道防线(ds4-server 不带登录, 将来谁把助理挂过去也不至于裸奔)。
 * 出错会怎样: 401/403 带一句原因。 */
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
    const bool tok = rq->signed_in || (asst_token[0] && !strncmp(rq->auth, "Bearer ", 7) && eq_const(rq->auth + 7, asst_token));
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

/* 请求体里的字符串数组 → 分隔表。name_list 只收 [a-z_] 的名字(能力/app 名, 别的字符进不了工具表匹配); path_list 收附件相对路径(不许 / ~ 开头、不许换行, 真伪由 asst_attach_line 再核) */
static void name_list(const char *body, const char *key, char *out, size_t n, char sep) {
    char *arr = NULL; size_t k = 0; out[0] = 0;
    if (!asst_jraw(body, key, &arr)) return;
    for (int i = 0; ; i++) {
        char *c = NULL; if (!asst_jarr_at(arr, i, &c)) break;
        char *v = NULL; const char *cp = c;
        if (json_string(&cp, &v) && v[0] && strspn(v, "abcdefghijklmnopqrstuvwxyz_") == strlen(v) && k + strlen(v) + 2 < n) { if (k) out[k++] = sep; memcpy(out + k, v, strlen(v) + 1); k += strlen(v); }
        free(v); free(c);
    }
    free(arr);
}
static void path_list(const char *body, char *out, size_t n) {
    char *arr = NULL; size_t k = 0; out[0] = 0;
    if (!asst_jraw(body, "files", &arr)) return;
    for (int i = 0; ; i++) {
        char *c = NULL; if (!asst_jarr_at(arr, i, &c)) break;
        char *v = NULL; const char *cp = c;
        if (json_string(&cp, &v) && v[0] && v[0] != '/' && v[0] != '~' && !strchr(v, '\n') && k + strlen(v) + 2 < n) { if (k) out[k++] = '\n'; memcpy(out + k, v, strlen(v) + 1); k += strlen(v); }
        free(v); free(c);
    }
    free(arr);
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
        char k[257]; const bool amap = asst_secret_get("amap_key", k, sizeof k); memset(k, 0, sizeof k);   /* 只报有没有, 值不出门 */
        ds4_buf_printf(out, ",\"local\":%s,\"amap_key\":%s,\"geo\":%s,\"nearby\":%s,\"toolbox\":", rq->peer_local ? "true" : "false", amap ? "true" : "false", asst_geo_ready() ? "true" : "false", asst_geo_nearby_ready() ? "true" : "false");
        asst_toolbox_json(out); ds4_buf_putc(out, '}');
        return 200;
    }
    if (!strcmp(r, "sessions")) { asst_sessions_json(out); return 200; }
    if (!strcmp(r, "memory")) {
        if (post) { if (!asst_memory_save(rq->body, err, sizeof err)) return jerr(out, 400, err); }
        asst_memory_json(out); return 200;
    }
    if (!strcmp(r, "skills")) {   /* 技能页: GET 列表(含 SKILL.md 原文与文件表); POST {op:save|delete, name, md} */
        if (post) { if (!asst_skill_op(rq->body, err, sizeof err)) return jerr(out, 400, err); }
        asst_skills_json(out); return 200;
    }
    if (!strcmp(r, "push")) {   /* 通知: GET {key, devices}; POST {op: subscribe|unsubscribe|test, …}(asst_push.c) */
        if (post) { if (!asst_push_op(rq->body, err, sizeof err)) return jerr(out, 400, err); }
        asst_push_json(out); return 200;
    }
    if (!strcmp(r, "reminders")) {   /* 定时提醒: GET 列表; POST {op: delete, id}(asst_notify.c) */
        if (post) { if (!asst_reminder_op(rq->body, err, sizeof err)) return jerr(out, 400, err); }
        asst_reminders_json(out); return 200;
    }
    if (!strcmp(r, "tasks")) {   /* 任务页: GET 列表; POST {op: add|pause|resume|done|run|delete, id, text}(asst_task.c) */
        if (post) { if (!asst_task_op(rq->body, err, sizeof err)) return jerr(out, 400, err); }
        asst_tasks_json(out); return 200;
    }
    if (!strcmp(r, "activity")) {
        ds4_buf_puts(out, "{\"audit\":"); audit_tail(out, 300);
        ds4_buf_puts(out, ",\"approvals\":"); asst_approvals_json(out);
        char *pj = asst_policy_json(); ds4_buf_printf(out, ",\"policy\":%s}", pj); free(pj);
        return 200;
    }
    if (!strcmp(r, "file")) {   /* 交付卡的下载/分享取字节(§13.2); session 只用来和页面的其它请求同形, 文件按工作区路径找 */
        char path[ASST_PATH]; asst_qget(rq->query, "path", path, sizeof path);
        return asst_file_get(path, out, ctype);
    }
    if (!post) return jerr(out, 405, "use POST");
    if (!strcmp(r, "policy")) { if (!asst_policy_op(rq->body, err, sizeof err)) return jerr(out, 400, err); ds4_buf_puts(out, "{\"ok\":true}"); return 200; }
    if (!strcmp(r, "presence")) {   /* 页面可见性心跳 {client, visible, endpoint}: 正看着页面的那台设备的推送先压着(asst_notify.c / asst_push.c) */
        char *client = NULL, *endpoint = NULL; bool vis = false; asst_jstr(rq->body, "client", &client); asst_jbool(rq->body, "visible", &vis); asst_jstr(rq->body, "endpoint", &endpoint);
        asst_presence_set(client ? client : "", vis, endpoint); free(client); free(endpoint);
        ds4_buf_puts(out, "{\"ok\":true}"); return 200;
    }
    if (!strcmp(r, "secret")) {   /* {name, value}: 凭证进 secrets/<name>(0600); 空 value = 删; 页面永远拿不回原文 */
        char *name = NULL, *value = NULL; asst_jstr(rq->body, "name", &name); asst_jstr(rq->body, "value", &value);
        const bool ok = asst_secret_set(name ? name : "", value, err, sizeof err);
        free(name); free(value);
        if (!ok) return jerr(out, 400, err);
        ds4_buf_puts(out, "{\"ok\":true}"); return 200;
    }
    if (!strcmp(r, "upload")) {   /* 用户主动给的附件(输入框旁的按钮): 原始体 = 文件字节 → workspace/inbox/; 发消息时把回的 path 放进 files, 事实行里就有 [附件: …] */
        char name[256]; asst_qget(rq->query, "name", name, sizeof name);
        char *msg = asst_file_save(name, rq->body, rq->body_len);
        if (!strncmp(msg, "error", 5)) { const int c = jerr(out, 400, msg); free(msg); return c; }
        const char *p = strstr(msg, "saved: "), *sp = p ? strchr(p + 7, ' ') : NULL;   /* "saved: inbox/x (N bytes)" → 只要路径 */
        ds4_buf_puts(out, "{\"path\":"); if (p && sp) { char path[300]; snprintf(path, sizeof path, "%.*s", (int)(sp - p - 7), p + 7); ds4_json_escape(out, path); } else ds4_buf_puts(out, "\"\"");
        ds4_buf_puts(out, ",\"note\":"); ds4_json_escape(out, msg); ds4_buf_putc(out, '}'); free(msg);
        return 200;
    }
    if (!strcmp(r, "skill_file")) {   /* 技能的随附文件(页面选整个文件夹, 每个文件一次): 原始体 = 文件字节, 技能名与相对路径在查询串里 */
        char name[ASST_SKILL_NAME_MAX + 8], path[512];
        asst_qget(rq->query, "name", name, sizeof name); asst_qget(rq->query, "path", path, sizeof path);
        char *why = asst_skill_put_file(name, path, rq->body, rq->body_len);
        if (why) { const int c = jerr(out, 400, why); free(why); return c; }
        ds4_buf_puts(out, "{\"ok\":true}"); return 200;
    }
    if (!strcmp(r, "device_file")) {   /* pick_file 的答复: 原始体 = 文件字节, 名字与请求 id 在查询串里 */
        char sid[ASST_ID_LEN + 1], id[64], name[256];
        asst_qget(rq->query, "session", sid, sizeof sid); asst_qget(rq->query, "id", id, sizeof id); asst_qget(rq->query, "name", name, sizeof name);
        if (!asst_id_ok(sid)) return jerr(out, 400, "bad session id");
        char *msg = asst_file_save(name, rq->body, rq->body_len);
        const bool ok = strncmp(msg, "error", 5) != 0;
        if (!asst_device_answer(sid, id, ok, msg, strlen(msg), err, sizeof err)) { free(msg); return jerr(out, 409, err); }
        ds4_buf_puts(out, "{\"ok\":"); ds4_json_escape(out, msg); ds4_buf_putc(out, '}'); free(msg);
        return 200;
    }
    char *sid = NULL; asst_jstr(rq->body, "session", &sid);
    int code = 200;
    if (!strcmp(r, "send")) {
        char *text = NULL, *client = NULL, *device = NULL, *kind = NULL;
        asst_jstr(rq->body, "text", &text); asst_jstr(rq->body, "client", &client); asst_jraw(rq->body, "device", &device);
        if (device) asst_jstr(device, "kind", &kind);
        char caps[256], apps[256], files[4096];
        name_list(rq->body, "caps", caps, sizeof caps, ','); name_list(rq->body, "apps", apps, sizeof apps, ',');   /* 页面报的能力表与 app 表: 只收 [a-z_] 的名字 */
        path_list(rq->body, files, sizeof files);
        const bool phone = kind && !strcmp(kind, "phone");
        char id[ASST_ID_LEN + 1];
        if (!text || !text[0]) code = jerr(out, 400, "empty message");
        else if (sid && sid[0] && !asst_id_ok(sid)) code = jerr(out, 400, "bad session id");
        else if (client && strlen(client) > 32) code = jerr(out, 400, "client id too long");
        else {
            if (sid && sid[0]) snprintf(id, sizeof id, "%s", sid);
            else { char t[64]; title_of(text, t, sizeof t); if (!asst_session_new(id, t, text, phone)) { free(text); free(sid); free(client); free(device); free(kind); return jerr(out, 500, "cannot create session"); } }
            bool ahead = false;
            if (!asst_run_submit(id, text, device, client, caps, apps, files, &ahead, err, sizeof err)) code = jerr(out, 409, err);
            else ds4_buf_printf(out, "{\"session\":\"%s\",\"queued\":%s}", id, ahead ? "true" : "false");
        }
        free(text); free(client); free(device); free(kind);
    } else if (!asst_id_ok(sid)) code = jerr(out, 400, "bad or missing session id");
    else if (!strcmp(r, "device")) {   /* 设备工具的文本答复(剪贴板内容 / 失败原因) */
        char *id = NULL, *data = NULL; bool ok = false;
        asst_jstr(rq->body, "id", &id); asst_jstr(rq->body, "data", &data); asst_jbool(rq->body, "ok", &ok);
        if (!asst_device_answer(sid, id ? id : "", ok, data ? data : "", data ? strlen(data) : 0, err, sizeof err)) code = jerr(out, 409, err); else ds4_buf_puts(out, "{\"ok\":true}");
        free(id); free(data);
    }
    else if (!strcmp(r, "stop")) {
        /* 谁按的停止也记进事件流(10-13 spark 实撞: 手机上刚发出去就"已停止", 盘上只有 end=stopped, 查不出停止请求从哪来) */
        char *client = NULL, *how = NULL; asst_jstr(rq->body, "client", &client); asst_jstr(rq->body, "how", &how);
        if (asst_id_ok(sid)) { ds4_buf e = {0}; ds4_buf_puts(&e, "{\"ev\":\"stop_request\",\"client\":"); ds4_json_escape(&e, client ? client : ""); ds4_buf_puts(&e, ",\"how\":"); ds4_json_escape(&e, how ? how : ""); ds4_buf_putc(&e, '}'); asst_ev(sid, e.ptr); ds4_buf_free(&e); }
        free(client); free(how);
        asst_run_stop(sid); ds4_buf_puts(out, "{\"ok\":true}");
    }
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
