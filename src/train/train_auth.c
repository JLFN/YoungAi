/* train_auth.c — 工作台登录(10-12 用户: "第一次使用必须设置用户名密码, 不要数据库, 加密存本地, 可以手动删除重置, 登录后密文存浏览器免登")。
 *
 * 盘上: ~/.ds4/studio/auth 一个 0600 文本文件, 三行:
 *   ds4-studio-auth 1
 *   pbkdf2-sha256 <轮数> <盐 32 hex> <哈希 64 hex>      哈希 = PBKDF2(用户名 + "\n" + 密码, 盐)
 *   session <会话密钥 64 hex>                           签浏览器票用, 建档时随机生成
 * 用户名和密码都只存单向哈希, 不存可还原的密文: 能还原就得把钥匙也放这台盘上, 拿到文件的人一样拿得到钥匙, 白加。
 * 用户名不单独存, 和密码合在一起哈希 —— 登录失败也就分不出是用户名错还是密码错, 只答一句"不对"。
 * ★重置 = 删掉这个文件★(rm ~/.ds4/studio/auth, 不用重启): 每条请求都重读它, 没了就回到"首次设置"; 会话密钥跟着没了, 所有浏览器的旧票一起作废。
 *
 * 浏览器(10-12 用户: "不要用 cookie, 用 localStorage"): 登录/设置成功在响应体里给票 {"session":"<签发秒>.<HMAC(会话密钥, 签发秒)>"},
 *   页面存进 localStorage 的 ds4.studio.session, 之后每条请求带 X-DS4-Session: <票>。30 天有效, 页面每次打开查 /api/auth/status 时
 *   票满一天就在响应里给新票(滑动续期), 天天用就一直免登。退出 = 页面删掉这一项, 服务端不用管。
 *   自定义头天然挡 CSRF 与 DNS 重绑定: 别的网站读不到本站的 localStorage, 跨站带自定义头又要预检(我们不答 CORS)。
 *   代价: 页面脚本读得到票, 页面若有 XSS 能偷走(cookie 的 HttpOnly 能挡这个) —— 页面里数据一律 textContent, 唯一 innerHTML 是先整段转义的 markdown。
 *
 * 闸(tr_auth_gate, train_http.c 在一切路由之前调): 只拦数据口 —— /api/…(/api/auth/… 除外)与 /metrics。静态页面(/、/monitor 等)放行:
 *   浏览器直接打开一个网址时没法带自定义头, 拦了页面就永远打不开; 页面本身不含数据, 数据都要再走被拦的接口。
 *   /v1/…(大模型的 OpenAI/Anthropic 兼容口, 10-12 用户: "用 API key 管理大模型的请求"): 外部客户端走不了网页登录, 认 API 密钥
 *   (train_keys.c, 页面"API"tab 建/删); 工作台自己的票与本机口令也认。都没有 → 401, 报错照 OpenAI 的格式, 客户端能直接显示。
 *   拦下的请求要么带有效票, 要么带 Authorization: Bearer <~/.ds4/assistant/token>(本机脚本用, 能读到这个 0600 文件的人本来就拥有这台机器);
 *   都没有就答 401 + X-DS4-Auth: setup|login 头(页面据此弹设置/登录框)。
 * 出错会怎样: 连错 TR_AUTH_MAX_FAIL 次锁 TR_AUTH_LOCK_S 秒, 期间登录一律 429(主进程单线程, 不能用 sleep 拖慢, 只能锁);
 *   auth 文件被改坏(不是上面三行) → 当成"未设置"会让人重设, 所以读不懂时答 500 并在日志说清, 不悄悄回到首次设置。 */
#include "train_internal.h"
#include "../common/ds4_sha256.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TR_AUTH_ITER 600000u          /* PBKDF2 轮数: OWASP 2023 对 PBKDF2-SHA256 的建议值; 本机一次实测 0.27 s, 偷走文件的人每猜一次也要这么久 */
#define TR_AUTH_SESSION_S (30L * 86400)   /* 票有效期 30 天 */
#define TR_AUTH_RENEW_S 86400L        /* 票满一天, 下次打开页面换新票(滑动续期) */
#define TR_AUTH_MAX_FAIL 5
#define TR_AUTH_LOCK_S 30

typedef enum { AUTH_SETUP, AUTH_READY, AUTH_BROKEN } auth_state;
typedef struct { uint32_t iter; uint8_t salt[16], hash[32], secret[32]; } auth_rec;

static char g_dir[TR_PATH], g_file[TR_PATH + 16], g_token[65];
static int g_fails;
static time_t g_locked_until;

void tr_auth_init(const char *dir, const char *machine_token) {
    if (dir) snprintf(g_dir, sizeof g_dir, "%s", dir);
    else { const struct passwd *pw = getpwuid(getuid()); snprintf(g_dir, sizeof g_dir, "%.1000s/.ds4/studio", pw && pw->pw_dir ? pw->pw_dir : "."); }
    snprintf(g_file, sizeof g_file, "%s/auth", g_dir);
    snprintf(g_token, sizeof g_token, "%s", machine_token ? machine_token : "");
    g_fails = 0; g_locked_until = 0;
}
const char *tr_auth_file(void) { return g_file; }

void tr_hex(const uint8_t *p, size_t n, char *out) { for (size_t i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", p[i]); }
static bool unhex(const char *s, uint8_t *p, size_t n) {
    if (strlen(s) != 2 * n) return false;
    for (size_t i = 0; i < n; i++) { unsigned v; if (sscanf(s + 2 * i, "%2x", &v) != 1) return false; p[i] = (uint8_t)v; }
    return true;
}
bool tr_eq_const(const uint8_t *a, const uint8_t *b, size_t n) {   /* 不在第一个不同字节提前返回: 比对时长不泄露前几位对没对 */
    uint8_t d = 0; for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]); return d == 0;
}
bool tr_rand(uint8_t *p, size_t n) {
    const int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return false;
    const bool ok = read(fd, p, n) == (ssize_t)n; close(fd); return ok;
}

static auth_state load(auth_rec *r) {
    size_t n = 0; errno = 0; char *s = tr_slurp(g_file, &n);
    if (!s) return errno == ENOENT ? AUTH_SETUP : AUTH_BROKEN;
    char salt[40] = "", hash[72] = "", sec[72] = ""; unsigned iter = 0; int ver = 0;
    const bool ok = sscanf(s, "ds4-studio-auth %d pbkdf2-sha256 %u %39s %71s session %71s", &ver, &iter, salt, hash, sec) == 5 && ver == 1 && iter > 0
                    && unhex(salt, r->salt, 16) && unhex(hash, r->hash, 32) && unhex(sec, r->secret, 32);
    free(s);
    r->iter = iter;
    if (!ok) fprintf(stderr, "ds4-train: 登录文件 %s 读不懂(被改过?) —— 想重设就删掉它\n", g_file);
    return ok ? AUTH_READY : AUTH_BROKEN;
}

static void derive(const char *user, const char *pass, const uint8_t salt[16], uint32_t iter, uint8_t out[32]) {
    ds4_buf b = {0}; ds4_buf_puts(&b, user); ds4_buf_putc(&b, '\n'); ds4_buf_puts(&b, pass);
    ds4_pbkdf2_sha256(b.ptr, b.len, salt, 16, iter, out, 32);
    memset(b.ptr, 0, b.len); ds4_buf_free(&b);
}

/* 校验规则(页面 web/studio.html 的 authCheck 同一套, 改一处要改两处): 返回错误码(页面按它出中英文提示), 合格返回 NULL */
const char *tr_auth_check(const char *user, const char *pass) {
    const size_t ul = strlen(user), pl = strlen(pass);
    if (ul < 3 || ul > 32) return "user_len";
    for (const char *p = user; *p; p++) if (!isalnum((unsigned char)*p) && !strchr("_.-", *p)) return "user_chars";
    if (pl < 8 || pl > 64) return "pass_len";
    int letter = 0, digit = 0, other = 0;
    for (const unsigned char *p = (const unsigned char *)pass; *p; p++) {
        if (*p < 0x20 || *p == 0x7f) return "pass_chars";
        if (isalpha(*p)) letter = 1; else if (isdigit(*p)) digit = 1; else other = 1;
    }
    if (letter + digit + other < 2) return "pass_weak";   /* 至少两类: 字母 / 数字 / 符号(含中文等非 ASCII) */
    if (!strcasecmp(user, pass)) return "pass_same";
    return NULL;
}

static bool save(const char *user, const char *pass) {
    auth_rec r; r.iter = TR_AUTH_ITER;
    if (!tr_rand(r.salt, 16) || !tr_rand(r.secret, 32)) return false;
    derive(user, pass, r.salt, r.iter, r.hash);
    char hs[33], hh[65], hk[65], tmp[TR_PATH + 32];
    tr_hex(r.salt, 16, hs); tr_hex(r.hash, 32, hh); tr_hex(r.secret, 32, hk);
    char parent[TR_PATH]; snprintf(parent, sizeof parent, "%s", g_dir);
    char *sl = strrchr(parent, '/'); if (sl && sl != parent) { *sl = 0; mkdir(parent, 0755); }
    mkdir(g_dir, 0700);
    snprintf(tmp, sizeof tmp, "%s.tmp", g_file);
    const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    char body[256]; const int n = snprintf(body, sizeof body, "ds4-studio-auth 1\npbkdf2-sha256 %u %s %s\nsession %s\n", r.iter, hs, hh, hk);
    const bool ok = write(fd, body, (size_t)n) == n && fsync(fd) == 0;
    close(fd);
    if (!ok || rename(tmp, g_file)) { unlink(tmp); return false; }   /* 先写临时再改名: 写到一半断电也不会留下半个文件让人被锁在外面 */
    return true;
}

static void ticket(const auth_rec *r, long ts, char out[96]) {
    char msg[48]; const int n = snprintf(msg, sizeof msg, "ds4-studio-session %ld", ts);
    uint8_t mac[32]; ds4_hmac_sha256(r->secret, 32, msg, (size_t)n, mac);
    char h[65]; tr_hex(mac, 32, h); snprintf(out, 96, "%ld.%s", ts, h);
}

/* 票有效返回签发时间, 无效返回 -1 */
static long ticket_ok(const auth_rec *r, const char *v) {
    char *dot = NULL; const long ts = strtol(v, &dot, 10);
    const long now = (long)time(NULL);
    if (!dot || *dot != '.' || ts <= 0 || ts > now + 60 || now - ts > TR_AUTH_SESSION_S) return -1;
    char want[96]; ticket(r, ts, want);
    const size_t n = strlen(want);
    return strlen(v) == n && tr_eq_const((const uint8_t *)v, (const uint8_t *)want, n) ? ts : -1;
}

static void put_ticket(ds4_buf *out, const auth_rec *r) {   /* 往 JSON 对象里接 ,"session":"<新票>" */
    char v[96]; ticket(r, (long)time(NULL), v);
    ds4_buf_printf(out, ",\"session\":\"%s\"", v);
}

static bool bearer_ok(const tr_req *rq) {
    if (!g_token[0] || strncmp(rq->auth, "Bearer ", 7)) return false;
    const size_t n = strlen(g_token);
    return strlen(rq->auth + 7) == n && tr_eq_const((const uint8_t *)rq->auth + 7, (const uint8_t *)g_token, n);
}

static int jerr(ds4_buf *out, int code, const char *e) { ds4_buf_puts(out, "{\"error\":"); ds4_json_escape(out, e); ds4_buf_putc(out, '}'); return code; }

/* 要票的路: 数据接口。页面不拦, /v1 另走 API 密钥(理由见文件头) */
static bool guarded(const char *p) { return (!strncmp(p, "/api/", 5) && strncmp(p, "/api/auth/", 10)) || !strcmp(p, "/metrics"); }

static int auth_route(const tr_req *rq, auth_state st, const auth_rec *r, ds4_buf *out) {
    const char *op = rq->path + 10;   /* "/api/auth/" 之后 */
    if (st == AUTH_BROKEN) return jerr(out, 500, "auth_broken");
    if (!strcmp(op, "status")) {
        const long ts = st == AUTH_READY && rq->sess[0] ? ticket_ok(r, rq->sess) : -1;
        const bool in = ts > 0 || (st == AUTH_READY && bearer_ok(rq));
        ds4_buf_printf(out, "{\"state\":\"%s\",\"file\":", st == AUTH_SETUP ? "setup" : in ? "ok" : "login");
        ds4_json_escape(out, g_file);
        if (ts > 0 && (long)time(NULL) - ts > TR_AUTH_RENEW_S) put_ticket(out, r);
        ds4_buf_putc(out, '}');
        return 200;
    }
    if (strcmp(rq->method, "POST")) return jerr(out, 405, "use POST");
    /* 自定义头: 跨站的"简单 POST"带不上它(带了就要预检, 我们不答 CORS), 别的网页没法替你抢先设密码 */
    if (strcmp(rq->xstudio, "1")) return jerr(out, 403, "missing X-DS4-Studio header");
    char user[128], pass[256];
    tr_body_get(rq, "user", user, sizeof user); tr_body_get(rq, "password", pass, sizeof pass);
    int code;
    if (!strcmp(op, "setup")) {
        const char *bad = tr_auth_check(user, pass);
        if (st != AUTH_SETUP) code = jerr(out, 409, "already_set");
        else if (bad) code = jerr(out, 400, bad);
        else if (!save(user, pass)) { fprintf(stderr, "ds4-train: 写不了登录文件 %s: %s\n", g_file, strerror(errno)); code = jerr(out, 500, "save_failed"); }
        else {
            auth_rec nr; if (load(&nr) != AUTH_READY) code = jerr(out, 500, "save_failed");
            else { ds4_buf_puts(out, "{\"ok\":true"); put_ticket(out, &nr); ds4_buf_putc(out, '}'); code = 200; fprintf(stderr, "ds4-train: 已设置登录, 存在 %s(删掉它 = 重置)\n", g_file); }
        }
    } else if (!strcmp(op, "login")) {
        const time_t now = time(NULL);
        if (st != AUTH_READY) code = jerr(out, 409, "not_set");
        else if (now < g_locked_until) code = jerr(out, 429, "locked");
        else {
            uint8_t h[32]; derive(user, pass, r->salt, r->iter, h);
            if (tr_eq_const(h, r->hash, 32)) { g_fails = 0; ds4_buf_puts(out, "{\"ok\":true"); put_ticket(out, r); ds4_buf_putc(out, '}'); code = 200; }
            else if (++g_fails >= TR_AUTH_MAX_FAIL) {
                g_fails = 0; g_locked_until = now + TR_AUTH_LOCK_S;
                fprintf(stderr, "ds4-train: 登录连错 %d 次, 锁 %d 秒\n", TR_AUTH_MAX_FAIL, TR_AUTH_LOCK_S);
                code = jerr(out, 429, "locked");
            } else code = jerr(out, 401, "bad_login");
        }
    } else code = jerr(out, 404, "unknown auth route");
    memset(pass, 0, sizeof pass);
    return code;
}

int tr_auth_gate(tr_req *rq, ds4_buf *out, ds4_buf *hdr) {
    auth_rec r; memset(&r, 0, sizeof r);
    const auth_state st = load(&r);
    int code = 0;
    if (!strncmp(rq->path, "/api/auth/", 10)) code = auth_route(rq, st, &r, out);
    else if (st == AUTH_READY && ((rq->sess[0] && ticket_ok(&r, rq->sess) > 0) || bearer_ok(rq))) rq->signed_in = true;
    else if (!strncmp(rq->path, "/v1/", 4)) {   /* 密钥与登录文件各管各的: 删登录文件重置密码, 外部客户端的密钥照样能用 */
        if (!tr_keys_check(rq)) {
            ds4_buf_puts(out, "{\"error\":{\"message\":\"Invalid or missing API key. Create one in the studio's API tab and send it as 'Authorization: Bearer <key>' (or 'x-api-key: <key>').\",\"type\":\"invalid_request_error\",\"code\":\"invalid_api_key\"}}");
            code = 401;
        }
    } else if (guarded(rq->path)) {
        ds4_buf_printf(hdr, "X-DS4-Auth: %s\r\n", st == AUTH_SETUP ? "setup" : "login");
        code = jerr(out, st == AUTH_BROKEN ? 500 : 401, st == AUTH_BROKEN ? "auth_broken" : "login required");
    }
    memset(&r, 0, sizeof r);
    return code;
}
