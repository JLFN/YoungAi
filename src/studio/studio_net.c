/* studio_net.c — 设置弹窗(10-12 用户: "菜单最下面加一个设置按钮, 点击弹窗填入云服务器的账号密码, 支持保存修改, 同样的本地文件存储;
 * 设置里面有个功能叫做穿透, 就是把本地的服务使用 frp 穿透到互联网借助云服务器; 还有一个上传证书的功能 … 最后实现互联网可以访问的 pwa")。
 *
 * 一张图:  手机 ──https──▶ 云服务器:公网口(frps)══ frp 隧道(TLS + 令牌, 套在 ssh -L 里)══▶ 本机 frpc ──http──▶ 127.0.0.1:<页面端口>(ds4-train)
 *   安全组只要放行 SSH 端口(本来就开)和公网端口: frps 控制口只听服务器本机, frpc 经 ssh 端口转发连它(studio_sshtun.c)。
 *   frps: 我们用 ssh(设置里的账号密码)装到云服务器, systemd 托管(ds4-frps.service), 服务器重启自己起来。流程在 studio_tunnel.c。
 *   frpc: ds4-train 的子进程(studio_frp.c)。有证书时 frpc 用 https2http 插件在本机解 TLS —— 证书私钥不离开这台机器, 云服务器只转字节。
 *   没证书也能穿: 公网口 = 页面端口, 走 http。能打开, 但浏览器只在 https 下给装 PWA。
 *   注意: 从隧道进来的请求在 ds4-train 看来都来自 127.0.0.1。登录闸(studio_auth.c)从不因为"来自本机"放行, 所以公网上照样要登录;
 *   助理接口那道"本机免口令"(asst_api.c)还要求 Host 是 localhost, 隧道带的是公网域名, 也放不进去。
 * 盘上(都在 ~/.ds4/studio/, 和登录文件同目录):
 *   cloud(0600)  "ds4-studio-cloud 1" 之后一行一项: host / port / user / pass <十六进制> / token <frp 令牌 64 hex> / tunnel 0|1 / pub <公网端口, 0 = 没设>
 *               / frp_port <frps 控制口, 0 = 7000> / frp_bind <SSH 转发在服务器那头连的地址, - = 127.0.0.1> / ext_token <复用的 frps 的令牌, 十六进制; 空 = 我们自己装的>(后两项部署时写, 换服务器清空)
 * 公网端口为什么可设(10-12 用户: "再加一个设置对外暴露的端口更合理"): 服务器 443 已被 nginx 之类占着时换个口就绕开;
 *   大陆服务器 + 没备案的域名, 云厂商拦 80/443(https 也按握手里的域名拦), 换成 8443 这类口一般不拦。PWA 只要求 https, 不要求 443。
 *   frp/         frp 二进制缓存、frpc.toml、frpc.log、部署日志 deploy.log;  tls/  证书(studio_cert.c)
 *   known_hosts  云服务器的 ssh 主机指纹(第一次连自动记; 服务器重装后连不上就删掉这个文件)
 *   ds4-askpass  指向 ds4-train 的符号链接, ssh 问密码时调它(studio_exec.c 文件头)
 * 密码为什么可还原地存(十六进制只为不怕空格换行, 不是加密): ssh 要用原文。加密就得把钥匙也放这台盘上, 拿到文件的人一样拿得到钥匙, 白加
 *   (同 studio_auth.c 的道理)。护住它的是 0600 权限; 接口永远不回传密码, 只回"存过没有"。
 * 出错会怎样: cloud 文件被改坏 → 接口答 500 cloud_broken, 不当成"没设置"悄悄覆盖掉。 */
#include "studio_internal.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

static char g_dir[TR_PATH], g_file[TR_PATH + 16];
static int g_port;

const char *tr_net_dir(void) { return g_dir; }

static void set_dir(const char *dir) {
    if (dir) snprintf(g_dir, sizeof g_dir, "%s", dir);
    else { const struct passwd *pw = getpwuid(getuid()); snprintf(g_dir, sizeof g_dir, "%.1000s/.ds4/studio", pw && pw->pw_dir ? pw->pw_dir : "."); }
    snprintf(g_file, sizeof g_file, "%s/cloud", g_dir);
}

static bool unhex(const char *s, char *out, size_t n) {
    const size_t l = strlen(s);
    if (l % 2 || l / 2 >= n) return false;
    for (size_t i = 0; i < l / 2; i++) { unsigned v; if (sscanf(s + 2 * i, "%2x", &v) != 1) return false; out[i] = (char)v; }
    out[l / 2] = 0;
    return true;
}

bool tr_cloud_load(tr_cloud *c) {
    memset(c, 0, sizeof *c); c->port = 22; snprintf(c->user, sizeof c->user, "root");
    errno = 0; size_t n = 0; char *s = tr_slurp(g_file, &n);
    if (!s) return errno == ENOENT;
    bool ok = !strncmp(s, "ds4-studio-cloud 1\n", 19);
    for (char *line = ok ? s + 19 : NULL, *nl; ok && line && *line; line = nl ? nl + 1 : NULL) {
        nl = strchr(line, '\n'); if (nl) *nl = 0;
        char k[16] = "", v[400] = "";
        if (!*line) continue;
        if (sscanf(line, "%15s %399s", k, v) < 1) { ok = false; break; }
        if (!strcmp(k, "host")) snprintf(c->host, sizeof c->host, "%s", v);
        else if (!strcmp(k, "port")) c->port = atoi(v);
        else if (!strcmp(k, "user")) snprintf(c->user, sizeof c->user, "%s", v);
        else if (!strcmp(k, "pass")) ok = unhex(v, c->pass, sizeof c->pass);
        else if (!strcmp(k, "token")) snprintf(c->token, sizeof c->token, "%s", v);
        else if (!strcmp(k, "tunnel")) c->tunnel = atoi(v) != 0;
        else if (!strcmp(k, "pub")) c->pub = atoi(v);
        else if (!strcmp(k, "frp_port")) c->frp_port = atoi(v);
        else if (!strcmp(k, "frp_bind")) snprintf(c->frp_bind, sizeof c->frp_bind, "%s", strcmp(v, "-") ? v : "");
        else if (!strcmp(k, "ext_token")) ok = !strcmp(v, "-") || unhex(v, c->ext_token, sizeof c->ext_token);
    }
    free(s);
    if (!ok) fprintf(stderr, "ds4-train: 云服务器设置 %s 读不懂(被改过?) —— 删掉它重填\n", g_file);
    return ok;
}

bool tr_cloud_save(const tr_cloud *c) {
    char ph[2 * sizeof c->pass + 1] = "", eh[2 * sizeof c->ext_token + 1] = "";   /* tr_hex 对空串一个字节都不写: 不先置空, 写进文件的是栈上垃圾(10-12 spark 实撞 cloud_broken) */
    tr_hex((const uint8_t *)c->pass, strlen(c->pass), ph); tr_hex((const uint8_t *)c->ext_token, strlen(c->ext_token), eh);
    ds4_buf b = {0};
    ds4_buf_printf(&b, "ds4-studio-cloud 1\nhost %s\nport %d\nuser %s\npass %s\ntoken %s\ntunnel %d\npub %d\nfrp_port %d\nfrp_bind %s\next_token %s\n",
                   c->host, c->port, c->user, ph, c->token, c->tunnel ? 1 : 0, c->pub, c->frp_port, c->frp_bind[0] ? c->frp_bind : "-", eh[0] ? eh : "-");
    char tmp[TR_PATH + 32]; snprintf(tmp, sizeof tmp, "%s.tmp", g_file);
    mkdir(g_dir, 0700);
    const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    const bool ok = fd >= 0 && write(fd, b.ptr, b.len) == (ssize_t)b.len && fsync(fd) == 0;
    if (fd >= 0) close(fd);
    memset(b.ptr, 0, b.len); ds4_buf_free(&b); memset(ph, 0, sizeof ph);
    if (!ok || rename(tmp, g_file)) { unlink(tmp); return false; }   /* 先写临时再改名: 半截文件会让设置变成"读不懂" */
    return true;
}

/* 地址与用户名会进 ssh 的 argv 和 frpc.toml: 只许主机名/IP 的字符, 且不许以 - 开头(否则 ssh 会把它当成选项, 比如 -oProxyCommand=…) */
static bool host_ok(const char *h) { const size_t l = strlen(h); return l && l <= 253 && isalnum((unsigned char)h[0]) && tr_clean(h, ".-:"); }
static bool user_ok(const char *u) { const size_t l = strlen(u); return l && l <= 32 && isalnum((unsigned char)u[0]) && tr_clean(u, "._-"); }

int tr_pub_port(const tr_cloud *c, bool tls, int page_port) { return c->pub > 0 ? c->pub : tls ? TR_HTTPS_PORT : page_port; }

int tr_askpass_main(const char *argv0, const char *prompt) {
    if (prompt && strstr(prompt, "yes/no")) return 1;   /* 主机指纹确认: 我们用 accept-new 不该走到这, 走到了就别乱答 */
    /* 设置目录 = 链接所在目录(ssh 按 SSH_ASKPASS 的全路径执行它): 主进程用的哪个目录, 这里读的就是哪个, 不另猜家目录 */
    char d[TR_PATH]; snprintf(d, sizeof d, "%s", argv0);
    char *sl = strrchr(d, '/'); if (!sl) return 1;
    *sl = 0; set_dir(d);
    tr_cloud c;
    if (!tr_cloud_load(&c) || !c.pass[0]) return 1;
    printf("%s\n", c.pass); fflush(stdout);   /* stdout 是 ssh 的管道(全缓冲): 不冲就可能一个字节都没写出去 */
    memset(&c, 0, sizeof c);
    return 0;
}

void tr_net_shutdown(void) { tr_frpc_stop(); tr_sshfwd_stop(); }

/* askpass 链接每次主进程起来时重建: ds4-train 换了位置(发布包解压到别处)旧链接就断了 */
static void relink_askpass(void) {
    char exe[TR_PATH] = "", ap[TR_PATH + 32]; snprintf(ap, sizeof ap, "%s/ds4-askpass", g_dir);
#ifdef __linux__
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1); if (n > 0) exe[n] = 0;
#elif defined(__APPLE__)
    char raw[TR_PATH]; uint32_t rs = sizeof raw;
    if (_NSGetExecutablePath(raw, &rs) || !realpath(raw, exe)) exe[0] = 0;
#endif
    mkdir(g_dir, 0700); unlink(ap);
    if (exe[0] && symlink(exe, ap)) fprintf(stderr, "ds4-train: 建不了 %s: %s\n", ap, strerror(errno));
}

void tr_net_init(const char *dir, int page_port) {
    set_dir(dir); g_port = page_port;
    char ap[TR_PATH + 32]; snprintf(ap, sizeof ap, "%s/ds4-askpass", g_dir);
    tr_exec_set_askpass(ap);
    relink_askpass();
    tr_cloud c;
    if (!tr_cloud_load(&c) || !c.tunnel) return;
    fprintf(stderr, "ds4-train: 穿透开着, 起 frpc(云服务器 %s)\n", c.host);
    tr_tunnel_start(page_port, false);
}

static int jerr(ds4_buf *out, int code, const char *e) { ds4_buf_puts(out, "{\"error\":"); ds4_json_escape(out, e); ds4_buf_putc(out, '}'); return code; }

static int status_json(ds4_buf *out, const tr_cloud *c) {
    ds4_buf_puts(out, "{\"file\":"); ds4_json_escape(out, g_file);
    ds4_buf_puts(out, ",\"server\":{\"host\":"); ds4_json_escape(out, c->host);
    ds4_buf_printf(out, ",\"port\":%d,\"user\":", c->port); ds4_json_escape(out, c->user);
    char crt[TR_PATH], key[TR_PATH]; const bool tls = tr_cert_present(crt, sizeof crt, key, sizeof key);
    ds4_buf_printf(out, ",\"has_password\":%s,\"pub\":%d},\"frp_reuse\":%s,\"ports\":{\"frp\":%d,\"public\":%d,\"default_https\":%d,\"default_http\":%d},",
                   c->pass[0] ? "true" : "false", c->pub, c->ext_token[0] ? "true" : "false", tr_frp_port(c), tr_pub_port(c, tls, g_port), TR_HTTPS_PORT, g_port);
    tr_tunnel_json(out, c, g_port); ds4_buf_putc(out, ',');
    tr_cert_json(out); ds4_buf_putc(out, '}');
    return 200;
}

int tr_net_api(const tr_req *rq, ds4_buf *out) {
    if (strncmp(rq->path, "/api/net/", 9)) return -1;
    const char *op = rq->path + 9;
    tr_cloud c;
    if (!tr_cloud_load(&c)) return jerr(out, 500, "cloud_broken");
    if (!strcmp(op, "status")) return status_json(out, &c);
    if (strcmp(rq->method, "POST")) return jerr(out, 405, "use POST");
    if (!strcmp(op, "save")) {
        char host[300], port[16], user[80], pass[256], pub[16];
        tr_body_get(rq, "host", host, sizeof host); tr_body_get(rq, "port", port, sizeof port); tr_body_get(rq, "pub", pub, sizeof pub);
        tr_body_get(rq, "user", user, sizeof user); tr_body_get(rq, "password", pass, sizeof pass);
        const int pn = port[0] ? atoi(port) : 22;
        if (!host_ok(host)) return jerr(out, 400, "bad_host");
        if (pn < 1 || pn > 65535) return jerr(out, 400, "bad_port");
        const int pb = pub[0] ? atoi(pub) : 0;   /* 空 = 按默认(443 / 页面端口) */
        if (strspn(pub, "0123456789") != strlen(pub) || pb > 65535 || (pub[0] && !pb)) return jerr(out, 400, "bad_pub");
        if (pb == TR_FRP_PORT) return jerr(out, 400, "pub_is_frp");   /* 7000 已经是 frps 自己的通信口 */
        if (!user_ok(user)) return jerr(out, 400, "bad_user");
        for (const unsigned char *p = (const unsigned char *)pass; *p; p++) if (*p < 0x20 || *p == 0x7f) return jerr(out, 400, "bad_pass");
        if (strlen(pass) >= sizeof c.pass) return jerr(out, 400, "bad_pass");
        if (!pass[0] && !c.pass[0]) return jerr(out, 400, "need_pass");
        /* 公网端口也算"换了": frps 的 allowPorts 与服务器防火墙要跟着改, 得重新部署 */
        const bool changed = strcmp(host, c.host) || pn != c.port || strcmp(user, c.user) || (pass[0] && strcmp(pass, c.pass)) || pb != c.pub;
        if (strcmp(host, c.host) || pn != c.port || strcmp(user, c.user)) { c.frp_port = 0; c.ext_token[0] = 0; c.frp_bind[0] = 0; }   /* 换了服务器: 上一台认出来的 frps 不作数, 部署时重新认 */
        snprintf(c.host, sizeof c.host, "%s", host); c.port = pn; c.pub = pb; snprintf(c.user, sizeof c.user, "%s", user);
        if (pass[0]) snprintf(c.pass, sizeof c.pass, "%s", pass);   /* 密码框留空 = 不改 */
        memset(pass, 0, sizeof pass);
        if (!c.token[0]) { uint8_t r[32]; if (!tr_rand(r, sizeof r)) return jerr(out, 500, "no_random"); tr_hex(r, sizeof r, c.token); }
        if (!tr_cloud_save(&c)) { fprintf(stderr, "ds4-train: 写不了 %s: %s\n", g_file, strerror(errno)); return jerr(out, 500, "save_failed"); }
        if (changed && c.tunnel) tr_tunnel_start(g_port, true);   /* 换了服务器: 穿透开着就按新服务器重新部署 */
        return status_json(out, &c);
    }
    if (!strcmp(op, "tunnel")) {
        char on[16]; tr_body_get(rq, "on", on, sizeof on);
        const bool want = !strcmp(on, "true") || !strcmp(on, "1");
        if (want && (!c.host[0] || !c.pass[0])) return jerr(out, 400, "no_server");
        c.tunnel = want;
        if (!tr_cloud_save(&c)) return jerr(out, 500, "save_failed");
        if (want) tr_tunnel_start(g_port, true); else tr_tunnel_stop();
        return status_json(out, &c);
    }
    if (!strcmp(op, "cert") || !strcmp(op, "cert_delete")) {
        int code = 200;
        if (op[4]) { if (!tr_cert_delete()) return jerr(out, 500, "delete_failed"); }
        else { code = tr_cert_upload(rq, out); if (code != 200) return code; out->len = 0; if (out->ptr) out->ptr[0] = 0; }
        if (c.tunnel) tr_tunnel_start(g_port, false);   /* 换了证书: frpc 按新配置重起(http ↔ https 换公网口), frps 不用动 */
        return status_json(out, &c);
    }
    return jerr(out, 404, "unknown net route");
}
