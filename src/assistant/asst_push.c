/* asst_push.c — 浏览器推送(Web Push, RFC 8030/8291/8292; 2026-10-14 用户: "加一个通知功能, 使用 pwa 推送")。
 * 一次性: ~/.ds4/assistant/push/vapid.json 里一对 P-256 密钥(VAPID, 推送服务凭它认这台机器)。页面开通知 = 把公钥交给浏览器订阅, 订阅(endpoint +
 * 浏览器那头的公钥 p256dh + 16 字节 auth)存 push/subs/<endpoint 的 sha256 前 16 位>.json, 一台设备一份。
 * 发一条 = 对每份订阅: 明文 JSON {title, body, url} 按 RFC 8291 加密(src/push) → curl POST 到 endpoint(推送服务: FCM / Mozilla / Apple), 头带
 * VAPID 的 JWT(aud = endpoint 的 origin, sub = 订阅时页面的 origin)。为什么用 curl 不自己连: endpoint 全是 https, 本项目不引 TLS 库; 下 frp、探公网口已经在用
 * curl(studio_frp.c / studio_tunnel.c), 这里同一个口径。推送服务答 404/410 = 订阅已失效(用户关了通知、换了浏览器), 当场删掉那份。
 * 发送在独立线程里跑(curl 一条 1~3 s, 几台设备串行), 调用方不等; 每条结果进 audit.jsonl。 */
#include "asst_internal.h"
#include "../common/ds4_sha256.h"
#include "../push/ds4_push.h"
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ASST_PUSH_TTL 86400     /* 设备离线时推送服务替我们存一天: 任务进展隔天看还有意义, 更久的就过时了 */
#define ASST_PUSH_CURL_S 20     /* 一条 curl 的墙钟: 推送服务正常 1 s 内答; 20 s 还没答就当这条没发出去, 别拖住后面的设备 */
#define ASST_PUSH_JWT_S 43200   /* JWT 有效 12 小时(RFC 8292 上限 24 小时); 每条现签, 不缓存 */
#define ASST_PUSH_SUBS 32       /* 最多记这么多台设备 */

static uint8_t g_priv[32], g_pub[65]; static char g_pub64[100]; static bool g_ready;
static pthread_mutex_t pmu = PTHREAD_MUTEX_INITIALIZER;

static void subs_dir(char *out, size_t n) { snprintf(out, n, "%s/push/subs", asst_root); }
static void sub_id(const char *endpoint, char id[17]) { uint8_t h[32]; ds4_sha256(endpoint, strlen(endpoint), h); for (int i = 0; i < 8; i++) snprintf(id + 2 * i, 3, "%02x", h[i]); }

void asst_push_init(void) {
    char d[ASST_PATH + 32]; snprintf(d, sizeof d, "%s/push", asst_root); mkdir(d, 0700); subs_dir(d, sizeof d); mkdir(d, 0700);
    char p[ASST_PATH + 32]; snprintf(p, sizeof p, "%s/push/vapid.json", asst_root);
    char *m = asst_slurp(p, NULL); char *pv = NULL; size_t n = 0;
    if (m && asst_jstr(m, "priv", &pv) && ds4_b64url_dec(pv, g_priv, 32, &n) && n == 32 && ds4_p256_pub(g_priv, g_pub)) g_ready = true;
    free(pv); free(m);
    if (!g_ready && ds4_p256_keygen(g_priv, g_pub)) {
        char pr[64], pb[100]; ds4_b64url_enc(g_priv, 32, pr); ds4_b64url_enc(g_pub, 65, pb);
        ds4_buf b = {0}; ds4_buf_printf(&b, "{\"priv\":\"%s\",\"pub\":\"%s\"}\n", pr, pb);
        g_ready = asst_write_file(p, b.ptr, b.len, 0600); ds4_buf_free(&b); memset(pr, 0, sizeof pr);
    }
    if (g_ready) ds4_b64url_enc(g_pub, 65, g_pub64);
}
const char *asst_push_key(void) { return g_ready ? g_pub64 : ""; }

typedef struct { char id[17]; char *endpoint, *p256dh, *auth, *label, *origin; long created; } sub_t;
static void sub_free(sub_t *s) { free(s->endpoint); free(s->p256dh); free(s->auth); free(s->label); free(s->origin); memset(s, 0, sizeof *s); }
static int subs_list(sub_t *rows, int max) {   /* 调用方拿着 pmu */
    char d[ASST_PATH + 32]; subs_dir(d, sizeof d);
    DIR *dir = opendir(d); int n = 0; struct dirent *e;
    while (dir && (e = readdir(dir)) && n < max) {
        if (strlen(e->d_name) != 21 || strcmp(e->d_name + 16, ".json")) continue;
        char p[ASST_PATH + 64]; snprintf(p, sizeof p, "%s/%s", d, e->d_name);
        char *m = asst_slurp(p, NULL); if (!m) continue;
        sub_t *s = &rows[n]; memset(s, 0, sizeof *s); snprintf(s->id, sizeof s->id, "%.16s", e->d_name);
        if (asst_jstr(m, "endpoint", &s->endpoint) && asst_jstr(m, "p256dh", &s->p256dh) && asst_jstr(m, "auth", &s->auth)) { asst_jstr(m, "label", &s->label); asst_jstr(m, "origin", &s->origin); asst_jint(m, "created", &s->created); n++; }
        else sub_free(s);
        free(m);
    }
    if (dir) closedir(dir);
    return n;
}
static void sub_unlink(const char *id) { char d[ASST_PATH + 32], p[ASST_PATH + 64]; subs_dir(d, sizeof d); snprintf(p, sizeof p, "%s/%s.json", d, id); unlink(p); }

static void proxy_get(char *out, size_t n);
void asst_push_json(ds4_buf *b) {
    char px[256]; proxy_get(px, sizeof px);
    ds4_buf_puts(b, "{\"key\":\""); ds4_buf_puts(b, asst_push_key()); ds4_buf_puts(b, "\",\"proxy\":"); ds4_json_escape(b, px); ds4_buf_puts(b, ",\"devices\":[");
    sub_t rows[ASST_PUSH_SUBS]; pthread_mutex_lock(&pmu); const int n = subs_list(rows, ASST_PUSH_SUBS); pthread_mutex_unlock(&pmu);
    for (int i = 0; i < n; i++) {
        ds4_buf_printf(b, "%s{\"id\":\"%s\",\"created\":%ld,\"label\":", i ? "," : "", rows[i].id, rows[i].created); ds4_json_escape(b, rows[i].label ? rows[i].label : "");
        ds4_buf_puts(b, ",\"origin\":"); ds4_json_escape(b, rows[i].origin ? rows[i].origin : ""); ds4_buf_putc(b, '}');
        sub_free(&rows[i]);
    }
    ds4_buf_puts(b, "]}");
}
int asst_push_devices(void) { sub_t rows[ASST_PUSH_SUBS]; pthread_mutex_lock(&pmu); const int n = subs_list(rows, ASST_PUSH_SUBS); pthread_mutex_unlock(&pmu); for (int i = 0; i < n; i++) sub_free(&rows[i]); return n; }

static int deliver(const char *title, const char *body, const char *url, bool hold_watched, int *held);

/* ---- 推送代理(10-15): 安卓 Chrome 的推送服务是谷歌 FCM, 国内直连超时(spark 实测 curl exit 28), 发送这头要走代理。
 * push/proxy 一行 URL, POST /api/assistant/push {op:proxy} 写(页面上填它的通知卡 10-15 删了); 空 = 直连(苹果 web.push.apple.com 国内直连就通)。不读 http_proxy 这类环境变量: 全仓零 env 配置。 */
static void proxy_path(char *out, size_t n) { snprintf(out, n, "%s/push/proxy", asst_root); }
static void proxy_get(char *out, size_t n) {
    char p[ASST_PATH + 32]; proxy_path(p, sizeof p); char *m = asst_slurp(p, NULL);
    snprintf(out, n, "%s", m ? m : ""); free(m); out[strcspn(out, "\r\n")] = 0;
}
static bool proxy_ok(const char *u) {   /* 只认 scheme://host:port, 字符白名单: 它进 curl 的 argv, 不许夹别的东西 */
    const bool scheme = !strncmp(u, "http://", 7) || !strncmp(u, "socks5://", 9) || !strncmp(u, "socks5h://", 10);
    return scheme && strlen(u) < 200 && strspn(u, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.:/-_[]") == strlen(u) && strchr(strstr(u, "://") + 3, ':');
}

static bool endpoint_ok(const char *e) {   /* 推送服务一律 https; http 只认本机(单测里的假推送服务) */
    return !strncmp(e, "https://", 8) || !strncmp(e, "http://127.0.0.1", 16) || !strncmp(e, "http://localhost", 16);
}

bool asst_push_op(const char *body, char *err, size_t errn) {
    if (!g_ready) { snprintf(err, errn, "push keys are not initialised"); return false; }
    char *op = NULL; asst_jstr(body, "op", &op);
    bool ok = false;
    if (!op) snprintf(err, errn, "op is required");
    else if (!strcmp(op, "subscribe")) {
        char *sub = NULL, *endpoint = NULL, *keys = NULL, *p256dh = NULL, *auth = NULL, *label = NULL, *origin = NULL;
        asst_jraw(body, "sub", &sub); asst_jstr(body, "label", &label); asst_jstr(body, "origin", &origin);
        if (sub) { asst_jstr(sub, "endpoint", &endpoint); if (asst_jraw(sub, "keys", &keys)) { asst_jstr(keys, "p256dh", &p256dh); asst_jstr(keys, "auth", &auth); } }
        uint8_t pk[70], ak[20]; size_t pn = 0, an = 0;
        if (!endpoint || !endpoint_ok(endpoint) || strlen(endpoint) > 2048) snprintf(err, errn, "subscription endpoint must be an https URL");
        else if (!p256dh || !ds4_b64url_dec(p256dh, pk, sizeof pk, &pn) || pn != 65 || pk[0] != 4) snprintf(err, errn, "p256dh must be a 65-byte uncompressed P-256 key");
        else if (!auth || !ds4_b64url_dec(auth, ak, sizeof ak, &an) || an != 16) snprintf(err, errn, "auth must be 16 bytes");
        else {
            char id[17]; sub_id(endpoint, id);
            ds4_buf b = {0};
            ds4_buf_puts(&b, "{\"endpoint\":"); ds4_json_escape(&b, endpoint); ds4_buf_puts(&b, ",\"p256dh\":"); ds4_json_escape(&b, p256dh); ds4_buf_puts(&b, ",\"auth\":"); ds4_json_escape(&b, auth);
            ds4_buf_puts(&b, ",\"label\":"); char lb[65]; snprintf(lb, sizeof lb, "%s", label ? label : ""); ds4_json_escape(&b, lb);
            ds4_buf_puts(&b, ",\"origin\":"); char og[201]; snprintf(og, sizeof og, "%s", origin ? origin : ""); ds4_json_escape(&b, og);
            ds4_buf_printf(&b, ",\"created\":%ld}\n", (long)time(NULL));
            char d[ASST_PATH + 32], p[ASST_PATH + 64]; subs_dir(d, sizeof d); snprintf(p, sizeof p, "%s/%s.json", d, id);
            pthread_mutex_lock(&pmu); ok = asst_write_file(p, b.ptr, b.len, 0600); pthread_mutex_unlock(&pmu);
            if (!ok) snprintf(err, errn, "cannot write the subscription");
            ds4_buf_free(&b);
        }
        free(sub); free(endpoint); free(keys); free(p256dh); free(auth); free(label); free(origin);
    } else if (!strcmp(op, "unsubscribe")) {
        char *endpoint = NULL, *id = NULL; asst_jstr(body, "endpoint", &endpoint); asst_jstr(body, "id", &id);
        char sid[17] = "";
        if (endpoint) sub_id(endpoint, sid); else if (id && strlen(id) == 16 && strspn(id, "0123456789abcdef") == 16) snprintf(sid, sizeof sid, "%s", id);
        if (!sid[0]) snprintf(err, errn, "endpoint or id is required"); else { pthread_mutex_lock(&pmu); sub_unlink(sid); pthread_mutex_unlock(&pmu); ok = true; }
        free(endpoint); free(id);
    } else if (!strcmp(op, "test")) {
        char *lang = NULL; asst_jstr(body, "lang", &lang);
        const bool zh = !lang || strcmp(lang, "en");
        const int n = deliver(zh ? "YoungAi 通知已打开" : "YoungAi notifications are on", zh ? "任务有进展、助理安排的提醒到点, 都会推到这里。" : "Task progress and the assistant's reminders will arrive here.", "/#tasks", false, NULL);
        if (n > 0) ok = true; else snprintf(err, errn, "no device received it (%s)", asst_push_devices() ? "the push service did not accept the message; see the audit log" : "no device has notifications turned on");
        free(lang);
    } else if (!strcmp(op, "proxy")) {   /* {op: proxy, url}: 空 url = 清掉, 直连 */
        char *u = NULL; asst_jstr(body, "url", &u); char p[ASST_PATH + 32]; proxy_path(p, sizeof p);
        if (!u || !u[0]) { pthread_mutex_lock(&pmu); unlink(p); pthread_mutex_unlock(&pmu); ok = true; }
        else if (!proxy_ok(u)) snprintf(err, errn, "proxy must look like http://127.0.0.1:7890 (http, socks5 or socks5h, with a port)");
        else { pthread_mutex_lock(&pmu); ok = asst_write_file(p, u, strlen(u), 0600); pthread_mutex_unlock(&pmu); if (!ok) snprintf(err, errn, "cannot write push/proxy"); }
        free(u);
    } else snprintf(err, errn, "unknown op");
    free(op);
    return ok;
}

/* 不经 shell 跑 curl, 收 stdout(-w 打的状态码); 返回 -1 = 起不来 */
static int run_curl(const char *const argv[], char *out, size_t on) {
    int pfd[2]; if (pipe(pfd)) return -1;
    const pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        dup2(pfd[1], 1); close(pfd[0]); close(pfd[1]);
        const int nul = open("/dev/null", O_RDWR); if (nul >= 0) { dup2(nul, 0); dup2(nul, 2); }
        execvp(argv[0], (char *const *)argv); _exit(127);
    }
    close(pfd[1]);
    size_t k = 0; ssize_t r;
    while ((r = read(pfd[0], out + k, on - 1 - k)) > 0) { k += (size_t)r; if (k >= on - 1) break; }
    out[k] = 0; close(pfd[0]);
    int st = 0; waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void origin_of(const char *url, char *out, size_t n) {   /* scheme://host[:port] */
    const char *p = strstr(url, "://"); p = p ? p + 3 : url;
    const char *e = strchr(p, '/'); snprintf(out, n, "%.*s", (int)(e ? e - url : (long)strlen(url)), url);
}

static bool send_one(const sub_t *s, const uint8_t *payload, size_t pn, char *status, size_t sn) {
    uint8_t ua_pub[70], auth[20]; size_t n1, n2;
    if (!ds4_b64url_dec(s->p256dh, ua_pub, sizeof ua_pub, &n1) || n1 != 65 || !ds4_b64url_dec(s->auth, auth, sizeof auth, &n2) || n2 != 16) { snprintf(status, sn, "bad keys"); return false; }
    uint8_t body[4200]; size_t bn;
    if (!ds4_webpush_encrypt(ua_pub, auth, NULL, NULL, payload, pn, body, &bn)) { snprintf(status, sn, "encrypt failed"); return false; }
    char aud[300], sub[256]; origin_of(s->endpoint, aud, sizeof aud);
    /* sub = 订阅时页面的 origin(https 时); 推送服务要一个能联系到运营者的地址, 本机工作台没有域名就给一个 mailto(RFC 8292 §2.1 只要求格式) */
    if (s->origin && !strncmp(s->origin, "https://", 8)) snprintf(sub, sizeof sub, "%s", s->origin); else snprintf(sub, sizeof sub, "mailto:youngai@localhost");
    char jwt[1200]; if (!ds4_vapid_jwt(aud, sub, (long)time(NULL) + ASST_PUSH_JWT_S, g_priv, jwt, sizeof jwt)) { snprintf(status, sn, "jwt failed"); return false; }
    char tmp[ASST_PATH + 64]; snprintf(tmp, sizeof tmp, "%s/push/.out-%ld-%s", asst_root, (long)getpid(), s->id);
    if (!asst_write_file(tmp, (const char *)body, bn, 0600)) { snprintf(status, sn, "cannot write body"); return false; }
    char authz[1400], ttl[32], data[ASST_PATH + 72]; snprintf(authz, sizeof authz, "Authorization: vapid t=%s, k=%s", jwt, g_pub64); snprintf(ttl, sizeof ttl, "TTL: %d", ASST_PUSH_TTL); snprintf(data, sizeof data, "@%s", tmp);
    char tmo[16], px[256]; snprintf(tmo, sizeof tmo, "%d", ASST_PUSH_CURL_S); proxy_get(px, sizeof px);
    /* 没填代理时 -x "" = curl 明说不用代理(也不吃进程环境里的 http_proxy); 本机地址(单测的假推送服务)一律不走代理 */
    const char *argv[] = { "curl", "-s", "-o", "/dev/null", "-w", "%{http_code}", "-m", tmo, "-x", px, "--noproxy", "127.0.0.1,localhost", "-X", "POST",
                           "-H", "Content-Type: application/octet-stream", "-H", "Content-Encoding: aes128gcm", "-H", ttl, "-H", "Urgency: normal", "-H", authz,
                           "--data-binary", data, s->endpoint, NULL };
    char out[64] = ""; const int rc = run_curl(argv, out, sizeof out);
    unlink(tmp);
    const int code = atoi(out);
    snprintf(status, sn, rc < 0 ? "curl not found" : rc ? "curl exit %d (%s)" : "http %d", rc < 0 ? 0 : rc ? rc : code, rc ? out : "");
    if (rc == 0 && (code == 404 || code == 410)) { snprintf(status, sn, "http %d: subscription gone, removed", code); sub_unlink(s->id); }
    return rc == 0 && code >= 200 && code < 300;
}

static void payload(ds4_buf *pl, const char *title, const char *body, const char *url) {   /* sw.js 收到的明文 {title, body, url} */
    ds4_buf_puts(pl, "{\"title\":"); ds4_json_escape(pl, title && title[0] ? title : "YoungAi");
    char cut[801]; snprintf(cut, sizeof cut, "%s", body ? body : ""); size_t c = strlen(cut); while (c && ((unsigned char)cut[c] & 0xC0) == 0x80) cut[c--] = 0;   /* 正文最多 800 字节, 退到字符边界 */
    ds4_buf_puts(pl, ",\"body\":"); ds4_json_escape(pl, cut);
    ds4_buf_puts(pl, ",\"url\":"); ds4_json_escape(pl, url ? url : "/"); ds4_buf_putc(pl, '}');
}
static void audit_push(const char *title, bool ok, const char *result) {
    ds4_buf a = {0}; ds4_buf_puts(&a, "{\"session\":\"\",\"tool\":\"push\",\"args\":"); ds4_json_escape(&a, title ? title : "");
    ds4_buf_printf(&a, ",\"decision\":\"auto\",\"ok\":%s,\"ms\":0,\"sandbox\":\"none\",\"result\":", ok ? "true" : "false"); ds4_json_escape(&a, result); ds4_buf_putc(&a, '}');
    asst_audit(a.ptr); ds4_buf_free(&a);
}
static const char *sub_name(const sub_t *s) { return s->label && s->label[0] ? s->label : s->id; }
static bool push_to(const sub_t *s, const char *title, const ds4_buf *pl) {
    char st[128], r[256]; const bool ok = send_one(s, (const uint8_t *)pl->ptr, pl->len, st, sizeof st);
    snprintf(r, sizeof r, "%.64s: %.100s", sub_name(s), st); audit_push(title, ok, r);
    return ok;
}

/* ---- 压着的(10-14 用户: "在 app 里什么都不弹, 离开后再推"): 哪台设备的页面正开着在看(在场表按订阅 id 认, asst_notify.c), 发给它的那条不推也不在页面里弹,
 * 追加进 push/held/<订阅 id>.jsonl; 它切后台/锁屏/心跳过期后, 通知线程的 asst_push_flush 按原样补推。只压那一台: 别的设备该推照推。 */
static void held_path(const char *id, char *out, size_t n) { snprintf(out, n, "%s/push/held/%.16s.jsonl", asst_root, id); }
static void hold(const sub_t *s, const char *title, const char *body, const char *url) {
    ds4_buf b = {0}; ds4_buf_printf(&b, "{\"t\":%ld,\"title\":", (long)time(NULL)); ds4_json_escape(&b, title ? title : "");
    ds4_buf_puts(&b, ",\"body\":"); ds4_json_escape(&b, body ? body : ""); ds4_buf_puts(&b, ",\"url\":"); ds4_json_escape(&b, url ? url : "/"); ds4_buf_puts(&b, "}\n");
    char d[ASST_PATH + 32], p[ASST_PATH + 64]; snprintf(d, sizeof d, "%s/push/held", asst_root); held_path(s->id, p, sizeof p);
    pthread_mutex_lock(&pmu); mkdir(d, 0700); const int fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0600); const bool ok = fd >= 0 && write(fd, b.ptr, b.len) == (ssize_t)b.len; if (fd >= 0) close(fd); pthread_mutex_unlock(&pmu);
    char r[256]; snprintf(r, sizeof r, ok ? "%.64s: held, the page is open on it now; pushed when it leaves the page" : "%.64s: cannot write push/held", sub_name(s)); audit_push(title, ok, r);
    ds4_buf_free(&b);
}

/* hold=false 只给"发一条测试": 那一下页面必然开着, 压着就测不到推送 */
static int deliver(const char *title, const char *body, const char *url, bool hold_watched, int *held) {
    if (held) *held = 0;
    if (!g_ready) return 0;
    sub_t rows[ASST_PUSH_SUBS]; pthread_mutex_lock(&pmu); const int n = subs_list(rows, ASST_PUSH_SUBS); pthread_mutex_unlock(&pmu);
    if (!n) audit_push(title, false, "not sent: no device has notifications turned on");   /* 到点的提醒没处去, 至少留一笔, 不静默丢 */
    ds4_buf pl = {0}; payload(&pl, title, body, url);
    const long now = (long)time(NULL); int sent = 0;
    for (int i = 0; i < n; i++) {
        if (hold_watched && asst_presence_watching(rows[i].id, now)) { hold(&rows[i], title, body, url); if (held) (*held)++; }
        else sent += push_to(&rows[i], title, &pl);
        sub_free(&rows[i]);
    }
    ds4_buf_free(&pl);
    return sent;
}
int asst_push_send_sync(const char *title, const char *body, const char *url, int *held) { return deliver(title, body, url, true, held); }

/* 通知线程每次醒来调: 页面已经不在看的设备, 把压着的按顺序推出去; 订阅已删的、压了超过推送服务存期(一天)的直接扔 */
void asst_push_flush(long now) {
    char d[ASST_PATH + 32]; snprintf(d, sizeof d, "%s/push/held", asst_root);
    DIR *dir = opendir(d); if (!dir) return;
    char ids[ASST_PUSH_SUBS * 2][17]; int k = 0; struct dirent *e;
    while ((e = readdir(dir)) && k < ASST_PUSH_SUBS * 2) if (strlen(e->d_name) == 22 && !strcmp(e->d_name + 16, ".jsonl")) snprintf(ids[k++], 17, "%.16s", e->d_name);
    closedir(dir);
    for (int i = 0; i < k; i++) {
        if (asst_presence_watching(ids[i], now)) continue;
        char p[ASST_PATH + 64]; held_path(ids[i], p, sizeof p);
        sub_t rows[ASST_PUSH_SUBS]; pthread_mutex_lock(&pmu);
        char *all = asst_slurp(p, NULL); unlink(p);   /* 读和删在同一把锁里: hold() 的追加不会夹在中间丢掉 */
        const int n = subs_list(rows, ASST_PUSH_SUBS); pthread_mutex_unlock(&pmu);
        const sub_t *s = NULL; for (int j = 0; j < n; j++) if (!strcmp(rows[j].id, ids[i])) s = &rows[j];
        for (char *line = all; s && line && *line; ) {
            char *nl = strchr(line, '\n'); if (nl) *nl = 0;
            long t = 0; char *ti = NULL, *bo = NULL, *ur = NULL;
            if (asst_jint(line, "t", &t) && now - t <= ASST_PUSH_TTL && asst_jstr(line, "title", &ti)) {
                asst_jstr(line, "body", &bo); asst_jstr(line, "url", &ur);
                ds4_buf pl = {0}; payload(&pl, ti, bo, ur); push_to(s, ti, &pl); ds4_buf_free(&pl);
            }
            free(ti); free(bo); free(ur);
            line = nl ? nl + 1 : NULL;
        }
        free(all); for (int j = 0; j < n; j++) sub_free(&rows[j]);
    }
}
void asst_push_sub_id(const char *endpoint, char id[17]) { if (endpoint && endpoint[0]) sub_id(endpoint, id); else id[0] = 0; }

typedef struct { char *title, *body, *url; } push_job;
static void *push_thread(void *arg) { push_job *j = arg; asst_push_send_sync(j->title, j->body, j->url, NULL); free(j->title); free(j->body); free(j->url); free(j); return NULL; }
void asst_push_send(const char *title, const char *body, const char *url) {
    if (!g_ready) return;
    push_job *j = calloc(1, sizeof *j); j->title = strdup(title ? title : ""); j->body = strdup(body ? body : ""); j->url = strdup(url ? url : "/");
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, push_thread, j)) { free(j->title); free(j->body); free(j->url); free(j); }
    pthread_attr_destroy(&at);
}
