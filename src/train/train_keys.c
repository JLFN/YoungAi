/* train_keys.c — 大模型 API 密钥(10-12 用户: "加一个 api 子 tab 用来管理, 使用 apikey 管理大模型的请求")。
 *
 * 管什么: 外部程序调 /v1/…(OpenAI / Anthropic 兼容口, 主进程转给模型子进程)必须带一把密钥, 放 Authorization: Bearer <密钥>
 *   或 x-api-key: <密钥>(Anthropic SDK 的放法)。闸在 train_auth.c 的 tr_auth_gate; 工作台自己的登录票与本机口令也放行。
 *   页面"API"tab 建、删、看用量(GET /api/keys/list, POST /api/keys/create|delete, 都要先登录)。
 *
 * 盘上: ~/.ds4/studio/apikeys(0600), 首行 "ds4-studio-apikeys 1", 之后一把一行:
 *   <id> <sha256(密钥) 64 hex> <创建秒> <最近使用秒(0 = 没用过)> <请求数> <显示用缩写> <名字(行尾, 可含空格)>
 * 只存哈希: 密钥只在创建那一刻完整给一次(页面提示抄走), 之后谁也看不到原文, 丢了就删掉重建。
 * 为什么用单轮 SHA-256 而登录密码用 60 万轮 PBKDF2: 密钥是 24 字节随机数(192 位), 拿到文件也没法穷举; 而且每条 /v1 请求都要验,
 *   跑 0.27 秒的 PBKDF2 会让每次请求凭空慢这么多。人挑的密码才短才可猜, 才要慢哈希。
 * 删掉一把 = 立刻失效(每条请求都重读这个文件, 不用重启); 删掉整个文件 = 全部失效。和登录文件互不影响: 重置密码不会打断外部客户端。
 * 出错会怎样: 文件被改坏 → 一把都不认(所有 /v1 请求 401, 日志说清), 不会把坏文件当"没有密钥"静默放行。 */
#include "train_internal.h"
#include "../common/ds4_sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TR_KEYS_MAX 64            /* 一台本机工作台, 64 把足够; 也给每条请求的线性比对封个顶 */
#define TR_KEY_PREFIX "sk-ds4-"   /* 带前缀: 贴到别处时一眼认得出是哪家的密钥, 泄露扫描工具也好认 */
#define TR_KEY_RAND 24            /* 随机字节数 → 48 个十六进制 */
#define TR_KEY_NAME 64

typedef struct { char id[17], display[32], name[TR_KEY_NAME + 8]; uint8_t hash[32]; long created, last_used, requests; } tr_key;

static char g_kdir[TR_PATH], g_kfile[TR_PATH + 16];

void tr_keys_init(const char *dir) {
    if (dir) snprintf(g_kdir, sizeof g_kdir, "%s", dir);
    else { const struct passwd *pw = getpwuid(getuid()); snprintf(g_kdir, sizeof g_kdir, "%.1000s/.ds4/studio", pw && pw->pw_dir ? pw->pw_dir : "."); }
    snprintf(g_kfile, sizeof g_kfile, "%s/apikeys", g_kdir);
}

static bool unhex32(const char *s, uint8_t out[32]) {
    if (strlen(s) != 64) return false;
    for (int i = 0; i < 32; i++) { unsigned v; if (sscanf(s + 2 * i, "%2x", &v) != 1) return false; out[i] = (uint8_t)v; }
    return true;
}

/* 返回把数; 没文件 0; 文件坏了 -1 */
static int load(tr_key *k) {
    errno = 0;
    size_t len = 0; char *s = tr_slurp(g_kfile, &len);
    if (!s) return errno == ENOENT ? 0 : -1;
    int n = 0; bool ok = !strncmp(s, "ds4-studio-apikeys 1\n", 21);
    for (char *line = ok ? s + 21 : NULL, *nl; ok && line && *line && n < TR_KEYS_MAX; line = nl ? nl + 1 : NULL) {
        nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (!*line) continue;
        char hh[72]; int off = 0; tr_key *e = &k[n];
        if (sscanf(line, "%16s %71s %ld %ld %ld %31s %n", e->id, hh, &e->created, &e->last_used, &e->requests, e->display, &off) != 6 || !unhex32(hh, e->hash)) { ok = false; break; }
        snprintf(e->name, sizeof e->name, "%s", line + off);
        n++;
    }
    free(s);
    if (!ok) { fprintf(stderr, "ds4-train: API 密钥文件 %s 读不懂(被改过?) —— 一把都不认; 删掉它就是清空全部密钥\n", g_kfile); return -1; }
    return n;
}

static bool save(const tr_key *k, int n) {
    char parent[TR_PATH]; snprintf(parent, sizeof parent, "%s", g_kdir);
    char *sl = strrchr(parent, '/'); if (sl && sl != parent) { *sl = 0; mkdir(parent, 0755); }
    mkdir(g_kdir, 0700);
    ds4_buf b = {0};
    ds4_buf_puts(&b, "ds4-studio-apikeys 1\n");
    for (int i = 0; i < n; i++) {
        char hh[65]; tr_hex(k[i].hash, 32, hh);
        ds4_buf_printf(&b, "%s %s %ld %ld %ld %s %s\n", k[i].id, hh, k[i].created, k[i].last_used, k[i].requests, k[i].display, k[i].name);
    }
    char tmp[TR_PATH + 32]; snprintf(tmp, sizeof tmp, "%s.tmp", g_kfile);
    const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool ok = fd >= 0 && write(fd, b.ptr, b.len) == (ssize_t)b.len;
    if (fd >= 0) close(fd);
    ds4_buf_free(&b);
    if (!ok || rename(tmp, g_kfile)) { unlink(tmp); return false; }   /* 先写临时再改名: 半截文件会让全部密钥失效 */
    return true;
}

bool tr_keys_check(const tr_req *rq) {
    const char *key = !strncmp(rq->auth, "Bearer ", 7) ? rq->auth + 7 : rq->xkey;
    if (strncmp(key, TR_KEY_PREFIX, strlen(TR_KEY_PREFIX))) return false;
    tr_key k[TR_KEYS_MAX];
    const int n = load(k);
    if (n <= 0) return false;
    uint8_t h[32]; ds4_sha256(key, strlen(key), h);
    for (int i = 0; i < n; i++) {
        if (!tr_eq_const(h, k[i].hash, 32)) continue;
        k[i].last_used = (long)time(NULL); k[i].requests++;
        if (!save(k, n)) fprintf(stderr, "ds4-train: 记不了 API 密钥用量(%s): %s\n", g_kfile, strerror(errno));   /* 用量没记上不挡请求 */
        return true;
    }
    return false;
}

static int jerr(ds4_buf *out, int code, const char *e) { ds4_buf_puts(out, "{\"error\":"); ds4_json_escape(out, e); ds4_buf_putc(out, '}'); return code; }

static void list_json(ds4_buf *out, const tr_key *k, int n) {
    ds4_buf_puts(out, "{\"keys\":[");
    for (int i = n - 1; i >= 0; i--) {   /* 新建的在前 */
        ds4_buf_printf(out, "%s{\"id\":\"%s\",\"display\":\"%s\",\"created\":%ld,\"last_used\":%ld,\"requests\":%ld,\"name\":", i == n - 1 ? "" : ",",
                       k[i].id, k[i].display, k[i].created, k[i].last_used, k[i].requests);
        ds4_json_escape(out, k[i].name); ds4_buf_putc(out, '}');
    }
    ds4_buf_puts(out, "],\"file\":"); ds4_json_escape(out, g_kfile); ds4_buf_putc(out, '}');
}

int tr_keys_api(const tr_req *rq, ds4_buf *out) {
    if (strncmp(rq->path, "/api/keys/", 10)) return -1;
    const char *op = rq->path + 10;
    tr_key k[TR_KEYS_MAX];
    const int n = load(k);
    if (n < 0) return jerr(out, 500, "keys_broken");
    if (!strcmp(op, "list")) { list_json(out, k, n); return 200; }
    if (strcmp(rq->method, "POST")) return jerr(out, 405, "use POST");
    if (!strcmp(op, "create")) {
        char raw[TR_KEY_NAME * 2]; tr_body_get(rq, "name", raw, sizeof raw);
        /* 名字进的是一行文本的行尾: 换行等控制字符换成空格, 首尾空白去掉; 按字节截断时别切坏 UTF-8 */
        char name[TR_KEY_NAME + 1]; size_t o = 0;
        for (const unsigned char *p = (const unsigned char *)raw; *p && o < TR_KEY_NAME; p++) name[o++] = *p < 0x20 || *p == 0x7f ? ' ' : (char)*p;
        if (o) {   /* 最后一个字符要是被截成半个就整个丢掉 */
            size_t i = o - 1; while (i && ((unsigned char)name[i] & 0xC0) == 0x80) i--;
            const unsigned char lead = (unsigned char)name[i];
            const size_t want = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
            if (i + want > o) o = i;
        }
        while (o && name[o - 1] == ' ') o--;
        name[o] = 0;
        const char *nm = name; while (*nm == ' ') nm++;
        if (!*nm) return jerr(out, 400, "name_empty");
        if (n >= TR_KEYS_MAX) return jerr(out, 409, "too_many");
        uint8_t rb[TR_KEY_RAND], idb[8];
        if (!tr_rand(rb, sizeof rb) || !tr_rand(idb, sizeof idb)) return jerr(out, 500, "no_random");
        char hexr[2 * TR_KEY_RAND + 1], key[64];
        tr_hex(rb, sizeof rb, hexr);
        snprintf(key, sizeof key, TR_KEY_PREFIX "%s", hexr);
        tr_key *e = &k[n]; memset(e, 0, sizeof *e);
        tr_hex(idb, sizeof idb, e->id);
        snprintf(e->display, sizeof e->display, TR_KEY_PREFIX "%.4s...%s", hexr, hexr + 2 * TR_KEY_RAND - 4);
        snprintf(e->name, sizeof e->name, "%s", nm);
        ds4_sha256(key, strlen(key), e->hash);
        e->created = (long)time(NULL);
        if (!save(k, n + 1)) { fprintf(stderr, "ds4-train: 写不了 API 密钥文件 %s: %s\n", g_kfile, strerror(errno)); return jerr(out, 500, "save_failed"); }
        ds4_buf_printf(out, "{\"id\":\"%s\",\"key\":\"%s\"}", e->id, key);   /* 原文只在这一次给出去 */
        memset(key, 0, sizeof key); memset(rb, 0, sizeof rb);
        return 200;
    }
    if (!strcmp(op, "delete")) {
        char id[32]; tr_body_get(rq, "id", id, sizeof id);
        int m = 0;
        for (int i = 0; i < n; i++) if (strcmp(k[i].id, id)) k[m++] = k[i];
        if (m == n) return jerr(out, 404, "no_such_key");
        if (!save(k, m)) return jerr(out, 500, "save_failed");
        ds4_buf_puts(out, "{\"ok\":true}");
        return 200;
    }
    return jerr(out, 404, "unknown keys route");
}
