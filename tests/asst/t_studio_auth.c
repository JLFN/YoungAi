/* t_studio_auth.c — 工作台登录(src/train/train_auth.c)与大模型 API 密钥(train_keys.c)单测: 校验规则、首次设置、票(X-DS4-Session 头)、
 * 闸放行/拦截、连错锁定、删文件重置; 密钥建/列/删、/v1 认 Bearer 与 x-api-key、用量计数、名字清洗、坏文件不放行。
 * 由 t_asst_sbx.c 的 main 调用(临时目录由它建), 返回失败条数。登录文件写在临时目录, 不碰真的 ~/.ds4/studio。
 * 每次登录/设置都要跑一遍 PBKDF2(60 万轮), 整组十来次, 秒级。 */
#include "../../src/train/train_internal.h"
#include <sys/stat.h>
#include <unistd.h>

static int fails, passes;
#define CHECKA(cond, ...) do { if (cond) passes++; else { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static tr_req mk(const char *method, const char *path, const char *body, const char *sess) {
    tr_req r; memset(&r, 0, sizeof r);
    snprintf(r.method, sizeof r.method, "%s", method); snprintf(r.path, sizeof r.path, "%s", path);
    r.body = body; r.body_len = body ? strlen(body) : 0;
    snprintf(r.xstudio, sizeof r.xstudio, "1"); snprintf(r.sess, sizeof r.sess, "%s", sess ? sess : "");
    return r;
}

/* 跑一条, 返回状态码; out/hdr 留给调用方看 */
static int run(tr_req *r, ds4_buf *out, ds4_buf *hdr) { out->len = 0; hdr->len = 0; if (out->ptr) out->ptr[0] = 0; if (hdr->ptr) hdr->ptr[0] = 0; return tr_auth_gate(r, out, hdr); }

static bool ticket_of(const ds4_buf *out, char *v, size_t n) {   /* 响应体里的 "session":"…" */
    const char *p = out->ptr ? strstr(out->ptr, "\"session\":\"") : NULL;
    if (!p) return false;
    p += strlen("\"session\":\"");
    snprintf(v, n, "%.*s", (int)strcspn(p, "\""), p);
    return v[0] != 0;
}

static void test_rules(void) {
    CHECKA(!tr_auth_check("admin", "abc12345"), "good pair");
    CHECKA(!strcmp(tr_auth_check("ab", "abc12345"), "user_len"), "user too short");
    CHECKA(!strcmp(tr_auth_check("a b c", "abc12345"), "user_chars"), "space in user");
    CHECKA(!strcmp(tr_auth_check("admin", "abc123"), "pass_len"), "pass too short");
    CHECKA(!strcmp(tr_auth_check("admin", "abcdefgh"), "pass_weak"), "letters only");
    CHECKA(!strcmp(tr_auth_check("admin", "12345678"), "pass_weak"), "digits only");
    CHECKA(!tr_auth_check("admin", "密码很长很长很长1"), "non-ASCII counts as a class");
    CHECKA(!strcmp(tr_auth_check("admin1234", "ADMIN1234"), "pass_same"), "pass == user (case-insensitive)");
    CHECKA(!strcmp(tr_auth_check("admin", "abc\x01" "12345"), "pass_chars"), "control char");
}

/* 密钥管理接口直接调 tr_keys_api(走到它之前闸已验过登录); /v1 的放行走闸 */
static int keys(const char *method, const char *op, const char *body, ds4_buf *out) {
    char path[64]; snprintf(path, sizeof path, "/api/keys/%s", op);
    tr_req r = mk(method, path, body, NULL); out->len = 0; if (out->ptr) out->ptr[0] = 0;
    return tr_keys_api(&r, out);
}
static int v1(const char *bearer, const char *xkey, ds4_buf *out, ds4_buf *hdr) {
    tr_req r = mk("POST", "/v1/messages", "{}", NULL);
    if (bearer) snprintf(r.auth, sizeof r.auth, "Bearer %s", bearer);
    if (xkey) snprintf(r.xkey, sizeof r.xkey, "%s", xkey);
    return run(&r, out, hdr);
}

static void test_keys(const char *dir, ds4_buf *out, ds4_buf *hdr) {
    char key[96] = "", id[32] = "", file[4500];
    snprintf(file, sizeof file, "%s/apikeys", dir);
    CHECKA(keys("GET", "list", NULL, out) == 200 && strstr(out->ptr, "\"keys\":[]"), "empty list: %s", out->ptr);
    CHECKA(keys("POST", "create", "{\"name\":\"  \"}", out) == 400 && strstr(out->ptr, "name_empty"), "blank name refused");
    CHECKA(keys("POST", "create", "{\"name\":\"my app\\nsecond line\"}", out) == 200, "create: %s", out->ptr);
    { const char *p = strstr(out->ptr, "\"key\":\""); if (p) snprintf(key, sizeof key, "%.*s", (int)strcspn(p + 7, "\""), p + 7);
      p = strstr(out->ptr, "\"id\":\""); if (p) snprintf(id, sizeof id, "%.*s", (int)strcspn(p + 6, "\""), p + 6); }
    CHECKA(!strncmp(key, "sk-ds4-", 7) && strlen(key) == 7 + 48, "key shape: %s", key);
    CHECKA(keys("GET", "list", NULL, out) == 200 && strstr(out->ptr, "my app second line") && !strstr(out->ptr, key), "list shows name, never the key: %s", out->ptr);
    FILE *fp = fopen(file, "rb"); char buf[2048] = ""; if (fp) { size_t n = fread(buf, 1, sizeof buf - 1, fp); buf[n] = 0; fclose(fp); }
    struct stat st; CHECKA(!stat(file, &st) && (st.st_mode & 0777) == 0600 && !strstr(buf, key + 7), "file 0600, key not on disk");

    CHECKA(v1(key, NULL, out, hdr) == 0, "/v1 with Bearer key");
    CHECKA(v1(NULL, key, out, hdr) == 0, "/v1 with x-api-key (Anthropic)");
    char bad[96]; snprintf(bad, sizeof bad, "%s", key); bad[20] = bad[20] == 'a' ? 'b' : 'a';
    CHECKA(v1(bad, NULL, out, hdr) == 401, "/v1 with a wrong key");
    CHECKA(keys("GET", "list", NULL, out) == 200 && strstr(out->ptr, "\"requests\":2"), "usage counted: %s", out->ptr);

    /* 名字按字节截到 64: 中文不能被截成半个字 */
    CHECKA(keys("POST", "create", "{\"name\":\"一二三四五六七八九十一二三四五六七八九十一二三四五六七八九十\"}", out) == 200, "long cjk name");
    CHECKA(keys("GET", "list", NULL, out) == 200 && strstr(out->ptr, "\"name\":\"一二三四五六七八九十一二三四五六七八九十一\""), "cut at a char boundary (21 chars = 63 bytes): %s", out->ptr);

    CHECKA(keys("POST", "delete", "{\"id\":\"nope\"}", out) == 404, "delete unknown id");
    CHECKA(keys("POST", "delete", NULL, out) == 404, "delete without body");
    char body[64]; snprintf(body, sizeof body, "{\"id\":\"%s\"}", id);
    CHECKA(keys("POST", "delete", body, out) == 200, "delete: %s", out->ptr);
    CHECKA(v1(key, NULL, out, hdr) == 401, "deleted key is dead at once");

    fp = fopen(file, "wb"); if (fp) { fputs("garbage\n", fp); fclose(fp); }
    CHECKA(keys("GET", "list", NULL, out) == 500, "broken keys file → 500");
    CHECKA(v1(key, NULL, out, hdr) == 401, "broken keys file lets nothing through");
    unlink(file);
}

int studio_auth_test(const char *base) {
    char dir[4400]; snprintf(dir, sizeof dir, "%s/studio", base);
    const char *tok = "0123456789abcdef0123456789abcdef";
    tr_auth_init(dir, tok); tr_keys_init(dir);
    ds4_buf out = {0}, hdr = {0};
    char ck[128] = "", bad[128];
    test_rules();

    /* 未设置: 静态页面放行, 数据接口 401 + X-DS4-Auth: setup, /v1 没密钥 401(OpenAI 格式) */
    tr_req r = mk("GET", "/api/train/status", NULL, NULL);
    CHECKA(run(&r, &out, &hdr) == 401 && strstr(hdr.ptr, "X-DS4-Auth: setup"), "unset api → 401 setup: %s", hdr.ptr ? hdr.ptr : "");
    r = mk("GET", "/", NULL, NULL); CHECKA(run(&r, &out, &hdr) == 0, "page is open");
    r = mk("POST", "/v1/chat/completions", "{}", NULL); CHECKA(run(&r, &out, &hdr) == 401 && strstr(out.ptr, "invalid_api_key"), "/v1 needs a key: %s", out.ptr);
    r = mk("GET", "/monitor", NULL, NULL); CHECKA(run(&r, &out, &hdr) == 0, "static monitor page is open");
    r = mk("GET", "/metrics", NULL, NULL); CHECKA(run(&r, &out, &hdr) == 401, "metrics data is guarded");
    r = mk("GET", "/api/auth/status", NULL, NULL); CHECKA(run(&r, &out, &hdr) == 200 && strstr(out.ptr, "\"setup\""), "status setup: %s", out.ptr);
    r = mk("GET", "/api/train/status", NULL, NULL); snprintf(r.auth, sizeof r.auth, "Bearer %s", tok);
    CHECKA(run(&r, &out, &hdr) == 401, "machine token does not bypass before setup");

    /* 设置 */
    const char *good = "{\"user\":\"owner\",\"password\":\"p4ss word!\"}";
    r = mk("POST", "/api/auth/setup", good, NULL); r.xstudio[0] = 0; CHECKA(run(&r, &out, &hdr) == 403, "setup without X-DS4-Studio");
    r = mk("POST", "/api/auth/setup", "{\"user\":\"owner\",\"password\":\"password\"}", NULL); CHECKA(run(&r, &out, &hdr) == 400 && strstr(out.ptr, "pass_weak"), "weak rejected: %s", out.ptr);
    r = mk("POST", "/api/auth/setup", good, NULL); CHECKA(run(&r, &out, &hdr) == 200 && ticket_of(&out, ck, sizeof ck), "setup ok + ticket: %s", out.ptr);
    CHECKA(!hdr.len, "no Set-Cookie or other headers: %s", hdr.ptr ? hdr.ptr : "");
    struct stat st; CHECKA(!stat(tr_auth_file(), &st) && (st.st_mode & 0777) == 0600, "auth file 0600");
    size_t n = 0; char *f = NULL; { FILE *fp = fopen(tr_auth_file(), "rb"); if (fp) { f = calloc(1, 512); n = fread(f, 1, 511, fp); fclose(fp); } }
    CHECKA(f && n > 0 && !strstr(f, "owner") && !strstr(f, "p4ss"), "no plaintext on disk: %s", f ? f : "(none)"); free(f);
    r = mk("POST", "/api/auth/setup", good, NULL); CHECKA(run(&r, &out, &hdr) == 409, "second setup refused");

    /* 票 */
    r = mk("GET", "/api/train/status", NULL, ck); CHECKA(run(&r, &out, &hdr) == 0 && r.signed_in, "ticket passes gate");
    snprintf(bad, sizeof bad, "%s", ck); bad[strlen(bad) - 1] ^= 1;
    r = mk("GET", "/api/train/status", NULL, bad); CHECKA(run(&r, &out, &hdr) == 401 && strstr(hdr.ptr, "X-DS4-Auth: login"), "tampered ticket");
    r = mk("GET", "/api/train/status", NULL, "1.deadbeef"); CHECKA(run(&r, &out, &hdr) == 401, "ancient forged ticket");
    r = mk("GET", "/api/auth/status", NULL, ck); CHECKA(run(&r, &out, &hdr) == 200 && strstr(out.ptr, "\"ok\"") && !strstr(out.ptr, "session"), "status ok, fresh ticket not renewed: %s", out.ptr);
    r = mk("GET", "/api/models/list", NULL, NULL); snprintf(r.auth, sizeof r.auth, "Bearer %s", tok);
    CHECKA(run(&r, &out, &hdr) == 0 && r.signed_in, "machine token passes after setup");
    r = mk("GET", "/api/models/list", NULL, NULL); snprintf(r.auth, sizeof r.auth, "Bearer %s", "0123456789abcdef0123456789abcdeX");
    CHECKA(run(&r, &out, &hdr) == 401, "wrong machine token");

    /* 登录 + 连错锁定 */
    r = mk("POST", "/api/auth/login", good, NULL); CHECKA(run(&r, &out, &hdr) == 200 && ticket_of(&out, ck, sizeof ck), "login ok");
    r = mk("POST", "/api/auth/login", "{\"user\":\"Owner\",\"password\":\"p4ss word!\"}", NULL); CHECKA(run(&r, &out, &hdr) == 401, "user is case-sensitive");
    for (int i = 0; i < 3; i++) { r = mk("POST", "/api/auth/login", "{\"user\":\"owner\",\"password\":\"nope1234\"}", NULL); CHECKA(run(&r, &out, &hdr) == 401 && strstr(out.ptr, "bad_login"), "wrong %d", i); }
    r = mk("POST", "/api/auth/login", "{\"user\":\"owner\",\"password\":\"nope1234\"}", NULL); CHECKA(run(&r, &out, &hdr) == 429, "5th wrong → locked");
    r = mk("POST", "/api/auth/login", good, NULL); CHECKA(run(&r, &out, &hdr) == 429, "locked even with the right password");
    tr_auth_init(dir, tok);   /* 重启主进程 = 锁清零 */
    r = mk("POST", "/api/auth/login", good, NULL); CHECKA(run(&r, &out, &hdr) == 200, "unlocked after restart");

    /* 删文件 = 重置: 回到首次设置, 旧票作废 */
    CHECKA(!unlink(tr_auth_file()), "rm auth file");
    r = mk("GET", "/api/train/status", NULL, ck); CHECKA(run(&r, &out, &hdr) == 401 && strstr(hdr.ptr, "X-DS4-Auth: setup"), "after rm: setup again");
    r = mk("POST", "/api/auth/setup", "{\"user\":\"owner2\",\"password\":\"another-pass9\"}", NULL); CHECKA(run(&r, &out, &hdr) == 200, "re-setup");
    r = mk("GET", "/api/train/status", NULL, ck); CHECKA(run(&r, &out, &hdr) == 401, "old ticket dead after reset");

    /* 文件被改坏: 不悄悄回到首次设置(否则谁都能重设), 答 500 */
    { FILE *fp = fopen(tr_auth_file(), "wb"); if (fp) { fputs("garbage\n", fp); fclose(fp); } }
    r = mk("GET", "/api/auth/status", NULL, NULL); CHECKA(run(&r, &out, &hdr) == 500, "broken file → 500");
    r = mk("POST", "/api/auth/setup", good, NULL); CHECKA(run(&r, &out, &hdr) == 500, "broken file: no re-setup");

    test_keys(dir, &out, &hdr);
    ds4_buf_free(&out); ds4_buf_free(&hdr);
    printf("studio auth: %d passed, %d failed\n", passes, fails);
    return fails;
}
