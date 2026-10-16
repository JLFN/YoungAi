/* sbx_policy.c — 守门的数据面: 路径解析、policy.json(你给的目录与 run 开关)、approvals.jsonl(永久授权)、会话内授权、"像密文"的识别。
 * 决策序本身在 sbx_guard.c。两张表都每次现读盘: 页面上一改立刻生效, 不存在"内存里还是旧规则"的窗口。
 * policy.json 形如 {"run": true, "dirs": [{"path": "/home/x/Documents", "write": false}]}; 没有这个文件 = 只有工作区, run 开(沙盒里跑, 不审批)。
 * ★包含助理根目录(~/.ds4/assistant)的目录不许加★: 加了家目录, run 就能 cat 到口令、改 policy 给自己开权限(Landlock 只能白名单, 挖不掉子目录)。 */
#include "../assistant/asst_internal.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

bool asst_under(const char *path, const char *root) {
    const size_t n = strlen(root);
    if (n == 1 && root[0] == '/') return path[0] == '/';
    return !strncmp(path, root, n) && (path[n] == 0 || path[n] == '/');
}

/* 先拼成绝对路径并按字面解 . 与 .., 再把"存在的最长前缀"realpath(软链就地解开), 不存在的尾巴原样接上(写新文件) */
bool asst_path_resolve(const char *in, char *out, size_t n) {
    char abs[ASST_PATH * 2];
    if (!in || !in[0]) snprintf(abs, sizeof abs, "%s", asst_ws);
    else if (in[0] == '~' && (in[1] == 0 || in[1] == '/')) snprintf(abs, sizeof abs, "%s%s", asst_home, in + 1);
    else if (in[0] == '/') snprintf(abs, sizeof abs, "%s", in);
    else snprintf(abs, sizeof abs, "%s/%s", asst_ws, in);
    char norm[ASST_PATH * 2] = ""; size_t nl = 0;
    for (char *s = abs, *tok; (tok = strsep(&s, "/")); ) {
        if (!tok[0] || !strcmp(tok, ".")) continue;
        if (!strcmp(tok, "..")) { while (nl && norm[nl - 1] != '/') nl--; if (nl) nl--; norm[nl] = 0; continue; }
        if (nl + strlen(tok) + 2 >= sizeof norm) return false;
        norm[nl++] = '/'; memcpy(norm + nl, tok, strlen(tok) + 1); nl += strlen(tok);
    }
    if (!nl) { norm[0] = '/'; norm[1] = 0; nl = 1; }
    char probe[ASST_PATH * 2]; snprintf(probe, sizeof probe, "%s", norm);
    char real[4096];
    for (size_t cut = strlen(probe);;) {
        if (realpath(probe, real)) {
            const char *rest = norm + cut;
            if (strlen(real) + strlen(rest) + 1 > n) return false;
            snprintf(out, n, "%s%s", strcmp(real, "/") || !rest[0] ? real : "", rest);
            return true;
        }
        char *sl = strrchr(probe, '/');
        if (!sl) return false;
        if (sl == probe) { probe[1] = 0; cut = 0; continue; }   /* 退到根还没有: realpath("/") 一定成功 */
        *sl = 0; cut = (size_t)(sl - probe);
    }
}

static bool dir_contains_root(const char *dir) { return asst_under(asst_root, dir); }

char *asst_policy_json(void) {
    char p[ASST_PATH + 16]; snprintf(p, sizeof p, "%s/policy.json", asst_root);
    char *s = asst_slurp(p, NULL);
    return s ? s : strdup("{\"run\":true,\"dirs\":[]}");
}

/* run 默认开(10-12 用户定: 沙盒里跑不审批); 只有 policy 里明写 "run": false 才关。老文件里的 "bash" 键不认 */
bool asst_policy_run(void) { char *p = asst_policy_json(); bool b = true; asst_jbool(p, "run", &b); free(p); return b; }

/* 第 i 个 policy 目录; 返回 false = 没有了。write 给 true/false */
bool asst_policy_dir(int i, char *path, size_t n, bool *write) {
    char *p = asst_policy_json(), *dirs = NULL, *d = NULL, *s = NULL;
    bool ok = asst_jraw(p, "dirs", &dirs) && asst_jarr_at(dirs, i, &d) && asst_jstr(d, "path", &s);
    if (ok) { snprintf(path, n, "%s", s); *write = false; asst_jbool(d, "write", write); }
    free(p); free(dirs); free(d); free(s);
    return ok;
}

static bool policy_write(bool run, const char *dirs_json) {
    ds4_buf b = {0}; ds4_buf_printf(&b, "{\"run\":%s,\"dirs\":%s}\n", run ? "true" : "false", dirs_json);
    char p[ASST_PATH + 16]; snprintf(p, sizeof p, "%s/policy.json", asst_root);
    const bool ok = asst_write_file(p, b.ptr, b.len, 0600);
    ds4_buf_free(&b);
    return ok;
}

/* ---- 永久授权 approvals.jsonl: {"t":…,"cls":"write","target":"/x/y"} 一行一条 ---- */
static bool approval_hit(const char *line, const char *cls, const char *path) {
    char *c = NULL, *t = NULL; bool hit = false;
    if (asst_jstr(line, "cls", &c) && asst_jstr(line, "target", &t)) hit = !strcmp(c, cls) && (!strcmp(cls, "web") ? !strcmp(t, path) : asst_under(path, t));
    free(c); free(t);
    return hit;
}

bool asst_approved_always(const char *cls, const char *path) {
    if (!strcmp(cls, "pay")) return false;   /* 交易永远逐次问(docs/assistant_approval.md): 手写进 approvals.jsonl 的也不认; 和守门的 once_only 各自成立, 哪层写错都不多放行 */
    char p[ASST_PATH + 24]; snprintf(p, sizeof p, "%s/approvals.jsonl", asst_root);
    char *all = asst_slurp(p, NULL); bool hit = false;
    for (char *line = all, *nl; line && *line && !hit; line = nl + 1) { nl = strchr(line, '\n'); if (nl) *nl = 0; hit = approval_hit(line, cls, path); if (!nl) break; }
    free(all);
    return hit;
}

void asst_approvals_json(ds4_buf *b) {
    char p[ASST_PATH + 24]; snprintf(p, sizeof p, "%s/approvals.jsonl", asst_root);
    char *all = asst_slurp(p, NULL); int i = 0;
    ds4_buf_putc(b, '[');
    for (char *line = all, *nl; line && *line; line = nl + 1) { nl = strchr(line, '\n'); if (nl) *nl = 0; if (*line == '{') { ds4_buf_puts(b, i++ ? "," : ""); ds4_buf_puts(b, line); } if (!nl) break; }
    ds4_buf_putc(b, ']');
    free(all);
}

/* ---- 会话内授权: 只在内存, 进程重启或会话结束就没了(这正是"本会话"的意思) ---- */
#define SESS_GRANTS 256
static struct { char sid[ASST_ID_LEN + 1]; char cls[8]; char target[ASST_PATH]; } g_sess[SESS_GRANTS];
static int g_nsess;
static pthread_mutex_t gmu = PTHREAD_MUTEX_INITIALIZER;

bool asst_approved_session(const char *sid, const char *cls, const char *path) {
    if (!strcmp(cls, "pay")) return false;
    bool hit = false;
    pthread_mutex_lock(&gmu);
    for (int i = 0; i < g_nsess && !hit; i++)
        hit = !strcmp(g_sess[i].sid, sid) && !strcmp(g_sess[i].cls, cls) && (!strcmp(cls, "web") ? !strcmp(g_sess[i].target, path) : asst_under(path, g_sess[i].target));
    pthread_mutex_unlock(&gmu);
    return hit;
}

void asst_guard_grant(const char *sid, const char *cls, const char *target, const char *scope) {
    if (!strcmp(scope, "session")) {
        pthread_mutex_lock(&gmu);
        const int i = g_nsess < SESS_GRANTS ? g_nsess++ : (int)(time(NULL) % SESS_GRANTS);   /* 满了覆盖一条: 最坏是多问一次, 不会多放行 */
        snprintf(g_sess[i].sid, sizeof g_sess[i].sid, "%s", sid); snprintf(g_sess[i].cls, sizeof g_sess[i].cls, "%s", cls); snprintf(g_sess[i].target, sizeof g_sess[i].target, "%s", target);
        pthread_mutex_unlock(&gmu);
    } else if (!strcmp(scope, "always")) {
        ds4_buf b = {0}; ds4_buf_printf(&b, "{\"t\":%ld,\"cls\":\"%s\",\"target\":", (long)time(NULL), cls); ds4_json_escape(&b, target);
        ds4_buf_printf(&b, ",\"session\":\"%s\"}\n", sid);
        char p[ASST_PATH + 24]; snprintf(p, sizeof p, "%s/approvals.jsonl", asst_root);
        FILE *f = fopen(p, "a"); if (f) { fputs(b.ptr, f); fclose(f); chmod(p, 0600); }
        ds4_buf_free(&b);
    }
}

/* 页面操作: {"op":"run","value":true} / {"op":"dir_add","path":…,"write":false} / {"op":"dir_del","path":…} / {"op":"revoke","index":N} */
bool asst_policy_op(const char *body, char *err, size_t errn) {
    char *op = NULL; bool ok = false;
    if (!asst_jstr(body, "op", &op)) { snprintf(err, errn, "missing op"); return false; }
    char *pj = asst_policy_json(), *dirs = NULL; bool run = true;
    asst_jbool(pj, "run", &run);
    if (!asst_jraw(pj, "dirs", &dirs)) dirs = strdup("[]");
    if (!strcmp(op, "run")) { bool v = true; asst_jbool(body, "value", &v); ok = policy_write(v, dirs); }
    else if (!strcmp(op, "dir_add") || !strcmp(op, "dir_del")) {
        char *in = NULL, real[ASST_PATH]; bool w = false; asst_jbool(body, "write", &w);
        const bool add = !strcmp(op, "dir_add");
        struct stat st;
        if (!asst_jstr(body, "path", &in) || !asst_path_resolve(in, real, sizeof real)) snprintf(err, errn, "bad path");
        else if (add && (stat(real, &st) || !S_ISDIR(st.st_mode))) snprintf(err, errn, "%s is not an existing directory", real);
        else if (add && (!strcmp(real, "/") || dir_contains_root(real))) snprintf(err, errn, "%s contains the assistant's own folder %s (token, policy); add a narrower folder such as ~/Documents", real, asst_root);
        else {
            ds4_buf nd = {0}; ds4_buf_putc(&nd, '['); int k = 0; char *d = NULL;
            for (int i = 0; asst_jarr_at(dirs, i, &d); i++, free(d), d = NULL) { char *s = NULL; asst_jstr(d, "path", &s); if (!s || strcmp(s, real)) { ds4_buf_puts(&nd, k++ ? "," : ""); ds4_buf_puts(&nd, d); } free(s); }
            if (add) { ds4_buf_puts(&nd, k++ ? "," : ""); ds4_buf_puts(&nd, "{\"path\":"); ds4_json_escape(&nd, real); ds4_buf_printf(&nd, ",\"write\":%s}", w ? "true" : "false"); }
            ds4_buf_putc(&nd, ']');
            ok = policy_write(run, nd.ptr);
            ds4_buf_free(&nd);
        }
        free(in);
    } else if (!strcmp(op, "revoke")) {
        long idx = -1; asst_jint(body, "index", &idx);
        char p[ASST_PATH + 24]; snprintf(p, sizeof p, "%s/approvals.jsonl", asst_root);
        char *all = asst_slurp(p, NULL); ds4_buf o = {0}; int i = 0;
        for (char *line = all, *nl; line && *line; line = nl + 1) { nl = strchr(line, '\n'); if (nl) *nl = 0; if (*line == '{' && i++ != idx) { ds4_buf_puts(&o, line); ds4_buf_putc(&o, '\n'); } if (!nl) break; }
        ok = asst_write_file(p, o.ptr ? o.ptr : "", o.len, 0600);
        ds4_buf_free(&o); free(all);
    } else snprintf(err, errn, "unknown op %s", op);
    free(op); free(pj); free(dirs);
    return ok;
}

/* ---- 像密文: 这些东西永远不该经过模型发到外面(网页参数、bash 命令) ---- */
static bool luhn(const char *d, int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) { int v = d[n - 1 - i] - '0'; if (i & 1) { v *= 2; if (v > 9) v -= 9; } sum += v; }
    return sum % 10 == 0;
}

bool asst_secret_like(const char *s) {
    if (!s) return false;
    if (strstr(s, "-----BEGIN")) return true;   /* PEM 私钥/证书 */
    if (asst_token[0] && strstr(s, asst_token)) return true;
    static const char *const kv[] = { "password=", "passwd=", "pwd=", "secret=", "api_key=", "apikey=", "access_token=", NULL };
    for (const char *p = s; *p; p++)
        for (int k = 0; kv[k]; k++) if (!strncasecmp(p, kv[k], strlen(kv[k]))) return true;
    for (const char *p = s; *p; ) {   /* 15~19 位连续数字且过 Luhn = 卡号形态; 13 位的毫秒时间戳不算 */
        if (!isdigit((unsigned char)*p)) { p++; continue; }
        const char *q = p; while (isdigit((unsigned char)*q)) q++;
        const int n = (int)(q - p);
        if (n >= 15 && n <= 19 && luhn(p, n)) return true;
        p = q;
    }
    return false;
}

/* 网址的主机是不是本机/内网: 浏览器能打到 127.0.0.1:8001(模型)与 :8000(工作台), 那是绕过鉴权的后门 */
bool asst_host_private(const char *host) {
    if (!host[0] || !strcasecmp(host, "localhost") || strstr(host, ".local") || strstr(host, ".internal")) return true;
    struct addrinfo hints, *res = NULL; memset(&hints, 0, sizeof hints); hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res)) return false;   /* 解析不了: 浏览器那边也去不了, 让它报错 */
    bool priv = false;
    for (struct addrinfo *a = res; a && !priv; a = a->ai_next) {
        if (a->ai_family == AF_INET) {
            const uint32_t ip = ntohl(((struct sockaddr_in *)a->ai_addr)->sin_addr.s_addr);
            priv = (ip >> 24) == 127 || (ip >> 24) == 10 || (ip >> 24) == 0 || (ip >> 16) == 0xC0A8 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xA9FE || (ip >> 22) == 0x191;   /* 100.64/10 */
        } else if (a->ai_family == AF_INET6) {
            const unsigned char *b = ((struct sockaddr_in6 *)a->ai_addr)->sin6_addr.s6_addr;
            static const unsigned char lo[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
            priv = !memcmp(b, lo, 16) || (b[0] & 0xFE) == 0xFC || (b[0] == 0xFE && (b[1] & 0xC0) == 0x80);
        }
    }
    freeaddrinfo(res);
    return priv;
}
