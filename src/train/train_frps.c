/* train_frps.c — 认云服务器上已有的东西(纯文本解析, 不连网, 单测在 tests/asst/t_studio_net.c)。用在 train_tunnel.c 的部署第一步。
 * 为什么要认(10-12 用户的服务器实撞): 那台服务器上已经跑着另一个项目的 frps 占着 7000。只填 IP 和账号就要能自己处理这类情况:
 *   - 7000 上是 frps: 读它的配置拿令牌, 放行端口也合适就直接复用 —— 服务器上什么都不装, 安全组也不用多开口;
 *   - 7000 上是别的程序, 或者 frps 的配置读不到/解析不了: 我们另装一套, 换一个空闲端口;
 *   - 公网端口被别的程序占着: 直接报是谁占的, 不等 frpc 起来才报 "port already used"。
 * 输入都是远端命令的原始输出: `ss -ltnpH`(谁在监听哪个口)、frps 进程的命令行、frps 的配置文件(新版 toml / 老版 ini 两种都认)。 */
#include "train_internal.h"
#include <ctype.h>

/* ss -ltnpH 一行: "LISTEN 0 4096 0.0.0.0:7000 0.0.0.0:* users:(("frps",pid=123,fd=3))"; IPv6 是 "[::]:7000" 或 "*:7000" */
int tr_listeners_parse(const char *ss, tr_listener *ls, int max) {
    int n = 0;
    for (const char *line = ss; line && *line && n < max; ) {
        const char *nl = strchr(line, '\n');
        const size_t len = nl ? (size_t)(nl - line) : strlen(line);
        char buf[1024]; snprintf(buf, sizeof buf, "%.*s", (int)(len < sizeof buf ? len : sizeof buf - 1), line);
        char st[16], rq[16], sq[16], local[256];
        if (sscanf(buf, "%15s %15s %15s %255s", st, rq, sq, local) == 4 && !strcmp(st, "LISTEN")) {
            const char *colon = strrchr(local, ':');
            const int port = colon ? atoi(colon + 1) : 0;
            if (port > 0) {
                tr_listener *l = &ls[n]; memset(l, 0, sizeof *l); l->port = port;
                char a[128]; snprintf(a, sizeof a, "%.*s", (int)(colon - local), local);
                char *pct = strchr(a, '%'); if (pct) *pct = 0;   /* 127.0.0.53%lo */
                if (a[0] == '[') { memmove(a, a + 1, strlen(a)); char *rb = strchr(a, ']'); if (rb) *rb = 0; }
                snprintf(l->addr, sizeof l->addr, "%s", !strcmp(a, "*") || !strcmp(a, "0.0.0.0") || !strcmp(a, "::") ? "127.0.0.1" : a);
                const char *u = strstr(buf, "users:((\"");
                if (u) { u += 9; snprintf(l->name, sizeof l->name, "%.*s", (int)strcspn(u, "\""), u); const char *pp = strstr(u, "pid="); if (pp) l->pid = atoi(pp + 4); }
                bool dup = false;   /* 同一个口 IPv4/IPv6 各一行 */
                for (int i = 0; i < n; i++) if (ls[i].port == port) { dup = true; if (!ls[i].name[0]) { snprintf(ls[i].name, sizeof ls[i].name, "%s", l->name); ls[i].pid = l->pid; } }
                if (!dup) n++;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    return n;
}

const tr_listener *tr_listener_on(const tr_listener *ls, int n, int port) {
    for (int i = 0; i < n; i++) if (ls[i].port == port) return &ls[i];
    return NULL;
}

/* frps 命令行里的配置路径: -c X / -c=X / --config X / --config=X; 没给 = 空串(frps 用内置默认: 7000、无令牌) */
bool tr_frps_conf_path(const char *cmdline, char *path, size_t n) {
    path[0] = 0;
    const char *keys[] = { "--config=", "-c=", "--config ", "-c " };
    for (int i = 0; i < 4; i++) {
        const char *p = strstr(cmdline, keys[i]);
        if (!p || (p != cmdline && p[-1] != ' ')) continue;
        p += strlen(keys[i]); while (*p == ' ') p++;
        snprintf(path, n, "%.*s", (int)strcspn(p, " \n"), p);
        return path[0] != 0;
    }
    return false;
}

static void trim(char *s) {
    char *e = s + strlen(s); while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    char *b = s; while (isspace((unsigned char)*b)) b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}
static void unquote(char *v) {   /* 去行尾注释与引号: token = "abc" # 注释 */
    trim(v);
    if (*v == '"' || *v == '\'') { const char q = *v; char *e = strchr(v + 1, q); if (e) *e = 0; memmove(v, v + 1, strlen(v)); return; }
    char *h = strchr(v, '#'); if (h) *h = 0;
    h = strchr(v, ';'); if (h) *h = 0;
    trim(v);
}

/* toml 的 allowPorts = [ { start = 2000, end = 3000 }, { single = 3001 } ](可能跨多行)→ "2000-3000,3001" */
static void toml_ports(const char *from, char *out, size_t n) {
    const char *lb = strchr(from, '['), *rb = lb ? strchr(lb, ']') : NULL;
    out[0] = 0; if (!lb || !rb) return;
    size_t o = 0;
    for (const char *p = lb; p < rb; ) {
        const char *ob = strchr(p, '{'); if (!ob || ob > rb) break;
        const char *cb = strchr(ob, '}'); if (!cb || cb > rb) break;
        char item[256]; snprintf(item, sizeof item, "%.*s", (int)(cb - ob), ob);
        const char *s = strstr(item, "single"), *a = strstr(item, "start"), *b = strstr(item, "end");
        int w = 0;
        if (s && strchr(s, '=')) w = snprintf(out + o, n - o, "%s%d", o ? "," : "", atoi(strchr(s, '=') + 1));
        else if (a && b && strchr(a, '=') && strchr(b, '=')) w = snprintf(out + o, n - o, "%s%d-%d", o ? "," : "", atoi(strchr(a, '=') + 1), atoi(strchr(b, '=') + 1));
        if (w > 0 && (size_t)w < n - o) o += (size_t)w;
        p = cb + 1;
    }
}

int tr_frps_parse(const char *text, int *bind_port, char *token, size_t tn, char *allow, size_t an) {
    *bind_port = TR_FRP_PORT; token[0] = 0; allow[0] = 0;   /* frps 不写这几项时的默认: 7000、无令牌、不限端口 */
    char section[64] = "";
    bool other_auth = false;
    for (const char *line = text; line && *line; ) {
        const char *nl = strchr(line, '\n');
        char buf[1024]; const size_t len = nl ? (size_t)(nl - line) : strlen(line);
        snprintf(buf, sizeof buf, "%.*s", (int)(len < sizeof buf ? len : sizeof buf - 1), line);
        trim(buf);
        if (buf[0] == '[' && buf[1] != '[') { snprintf(section, sizeof section, "%.*s", (int)strcspn(buf + 1, "]"), buf + 1); }
        else if (buf[0] && buf[0] != '#' && buf[0] != ';' && strchr(buf, '=')) {
            char k[128], v[800]; char *eq = strchr(buf, '=');
            snprintf(k, sizeof k, "%.*s", (int)(eq - buf), buf); trim(k);
            snprintf(v, sizeof v, "%s", eq + 1);
            /* 段内的键拼成点号全名: toml 写 [auth] 下面 token = …, 与 auth.token = … 同义; ini 的 [common] 段不拼 */
            char full[200];
            if (section[0] && strcmp(section, "common")) snprintf(full, sizeof full, "%s.%s", section, k); else snprintf(full, sizeof full, "%s", k);
            if (!strcmp(full, "allowPorts")) toml_ports(strstr(line, "allowPorts"), allow, an);   /* 数组可能跨多行: 从原文这一行往后找到 ] */
            else {
                unquote(v);
                if (!strcmp(full, "bindPort") || !strcmp(full, "bind_port")) *bind_port = atoi(v);
                else if (!strcmp(full, "auth.token") || !strcmp(full, "token")) snprintf(token, tn, "%s", v);
                else if (!strcmp(full, "allow_ports")) { snprintf(allow, an, "%s", v); for (char *p = allow; *p; p++) if (*p == ' ') memmove(p, p + 1, strlen(p)); }
                else if ((!strcmp(full, "auth.method") || !strcmp(full, "authentication_method")) && strcmp(v, "token")) other_auth = true;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    if (other_auth) return -1;   /* oidc 之类: 我们只会令牌认证 */
    return *bind_port > 0 ? 0 : -1;
}

bool tr_ports_allow(const char *allow, int port) {
    if (!allow || !allow[0]) return true;
    for (const char *p = allow; *p; ) {
        char *e = NULL; const long a = strtol(p, &e, 10);
        long b = a;
        if (e && *e == '-') b = strtol(e + 1, &e, 10);
        if (port >= a && port <= b) return true;
        p = e && *e == ',' ? e + 1 : "";
    }
    return false;
}
