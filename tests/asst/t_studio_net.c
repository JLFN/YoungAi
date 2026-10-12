/* t_studio_net.c — 设置弹窗(src/train/train_net.c 一族)离线单测: 云服务器设置的校验/存取/不回传密码/0600、askpass 读同目录密码、
 * 证书 zip 的认法(带链优先、不配对拒、过期拒、不是 zip 拒、带密码私钥拒)。不连任何服务器; 要 openssl 与 zip 两个命令造夹具。
 * 真 ssh 部署 + frpc + 公网口那条链在 tests/net/tunnel_e2e.sh(要一台服务器)。由 t_asst_sbx.c 的 main 调用, 返回失败条数。 */
#include "../../src/train/train_internal.h"
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails, passes;
#define CHECKN(cond, ...) do { if (cond) passes++; else { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int call(const char *path, const char *body, size_t len, ds4_buf *out) {
    tr_req r; memset(&r, 0, sizeof r);
    snprintf(r.method, sizeof r.method, "%s", body ? "POST" : "GET"); snprintf(r.path, sizeof r.path, "%s", path);
    r.body = body; r.body_len = body ? (len ? len : strlen(body)) : 0;
    out->len = 0; if (out->ptr) out->ptr[0] = 0;
    return tr_net_api(&r, out);
}
static bool sh(const char *dir, const char *const argv[]) {   /* 在 dir 里跑一条造夹具的命令 */
    char cwd[4096]; if (!getcwd(cwd, sizeof cwd) || chdir(dir)) return false;
    const int rc = tr_exec(argv, NULL, 0, NULL, 30, 0);
    return !chdir(cwd) && rc == 0;
}
static int upload(const char *zip, ds4_buf *out) {
    size_t n = 0; char *z = tr_slurp(zip, &n);
    if (!z) { out->len = 0; return -1; }
    const int c = call("/api/net/cert", z, n, out); free(z); return c;
}

/* 认服务器上已有东西的纯文本解析(train_frps.c): ss 输出、frps 命令行、toml / ini 两种配置、放行端口 */
static void test_frps_parse(void) {
    const char *ss =
        "LISTEN 0      4096         0.0.0.0:22        0.0.0.0:*    users:((\"sshd\",pid=812,fd=3))\n"
        "LISTEN 0      4096               *:7000            *:*    users:((\"frps\",pid=1234,fd=3))\n"
        "LISTEN 0      4096            [::]:7000         [::]:*    users:((\"frps\",pid=1234,fd=4))\n"
        "LISTEN 0      511          0.0.0.0:443       0.0.0.0:*    users:((\"nginx\",pid=900,fd=6),(\"nginx\",pid=901,fd=6))\n"
        "LISTEN 0      4096       127.0.0.53%lo:53        0.0.0.0:*    users:((\"systemd-resolve\",pid=600,fd=14))\n";
    tr_listener ls[16]; const int n = tr_listeners_parse(ss, ls, 16);
    const tr_listener *l7 = tr_listener_on(ls, n, 7000), *l443 = tr_listener_on(ls, n, 443);
    CHECKN(n == 4 && l7 && !strcmp(l7->name, "frps") && l7->pid == 1234, "ss: 7000 = frps 1234 (v4/v6 deduped), n=%d", n);
    CHECKN(!strcmp(l7->addr, "127.0.0.1") && !strcmp(tr_listener_on(ls, n, 53)->addr, "127.0.0.53"), "listen addr: wildcard → 127.0.0.1, specific kept: %s", tr_listener_on(ls, n, 53)->addr);
    CHECKN(l443 && !strcmp(l443->name, "nginx") && l443->pid == 900 && !tr_listener_on(ls, n, 8443), "ss: 443 = nginx, 8443 free");
    char path[256];
    CHECKN(tr_frps_conf_path("/usr/local/frp/frps -c /usr/local/frp/frps.toml", path, sizeof path) && !strcmp(path, "/usr/local/frp/frps.toml"), "conf -c: %s", path);
    CHECKN(tr_frps_conf_path("./frps --config=frps.ini ", path, sizeof path) && !strcmp(path, "frps.ini"), "conf --config=: %s", path);
    CHECKN(!tr_frps_conf_path("/usr/bin/frps", path, sizeof path), "no -c");
    int port; char tok[200], allow[256];
    CHECKN(!tr_frps_parse("bindPort = 7000\nauth.method = \"token\"\nauth.token = \"Ab#1 x\"  # 注释\n", &port, tok, sizeof tok, allow, sizeof allow)
           && port == 7000 && !strcmp(tok, "Ab#1 x") && !allow[0], "toml dotted: [%s]", tok);
    CHECKN(!tr_frps_parse("bindPort = 7100\nallowPorts = [\n  { start = 2000, end = 3000 },\n  { single = 8443 }\n]\n[auth]\ntoken = 'sec'\n", &port, tok, sizeof tok, allow, sizeof allow)
           && port == 7100 && !strcmp(tok, "sec") && !strcmp(allow, "2000-3000,8443"), "toml [auth] + multiline allowPorts: %d %s %s", port, tok, allow);
    CHECKN(!tr_frps_parse("[common]\nbind_port = 7000\ntoken = legacy123\nallow_ports = 2000-3000, 3001\n", &port, tok, sizeof tok, allow, sizeof allow)
           && port == 7000 && !strcmp(tok, "legacy123") && !strcmp(allow, "2000-3000,3001"), "ini: %s %s", tok, allow);
    CHECKN(tr_frps_parse("bindPort = 7000\nauth.method = \"oidc\"\n", &port, tok, sizeof tok, allow, sizeof allow) == -1, "oidc not reusable");
    CHECKN(tr_ports_allow("", 8443) && tr_ports_allow("2000-3000,8443", 8443) && tr_ports_allow("2000-3000,8443", 2500) && !tr_ports_allow("2000-3000,3001", 8443), "allow ranges");
}

/* frpc 日志哪些行算连接状态(train_frp.c 的 tr_frpc_line_state)。行取自 10-12 spark 的 frpc.log(带颜色控制码; 对端地址换成了文档专用段 203.0.113.0/24):
   请求级的告警与 TLS 握手报错曾被当成断线, 页面在隧道好好的时候显示"重连中" */
static void test_frpc_lines(void) {
    CHECKN(tr_frpc_line_state("\x1b[1;34m2026-10-12 12:18:04.315 [I] [client/control.go:174] [3cdca164a3663e7e] [ds4-studio] start proxy success") == 2, "proxy success = up");
    CHECKN(tr_frpc_line_state("2026-10-12 12:18:04.120 [I] [client/service.go:332] [3cdca164a3663e7e] login to server success, get run id [3cdca164a3663e7e]") == 1, "login = connecting");
    CHECKN(tr_frpc_line_state("2026-10-12 12:18:03.900 [I] [client/service.go:294] try to connect to server...") == 1, "try to connect = connecting");
    CHECKN(tr_frpc_line_state("\x1b[0m\x1b[1;33m2026-10-12 13:02:08.793 [W] [httputil/reverseproxy.go:689] http: proxy error: context canceled") == 0, "browser-cancelled request is not a disconnect");
    CHECKN(tr_frpc_line_state("2026/10/12 12:56:55 http: TLS handshake error from 203.0.113.7:56146: client sent an HTTP request to an HTTPS server") == 0, "TLS handshake noise ignored");
    CHECKN(tr_frpc_line_state("2026-10-12 14:00:00.000 [I] [proxy/proxy_manager.go:180] [3cdca164a3663e7e] proxy added: [ds4-studio]") == 0, "proxy added is not a state");
    CHECKN(tr_frpc_line_state("2026-10-12 14:00:01.000 [W] [client/service.go:300] connect to server error: dial tcp 127.0.0.1:17000: connect: connection refused") == -1, "connect error = down");
    CHECKN(tr_frpc_line_state("2026-10-12 14:00:02.000 [W] [client/control.go:168] [ds4-studio] start error: port already used") == -1, "proxy start error = down");
}

int studio_net_test(const char *base) {
    test_frps_parse();
    test_frpc_lines();
    char dir[4200], fx[4200], p[4400];
    snprintf(dir, sizeof dir, "%s/net-studio", base); snprintf(fx, sizeof fx, "%s/net-fx", base);
    mkdir(dir, 0700); mkdir(fx, 0700);
    tr_net_init(dir, 18000);
    ds4_buf out = {0};

    /* 设置: 校验 */
    CHECKN(call("/api/net/status", NULL, 0, &out) == 200 && strstr(out.ptr, "\"user\":\"root\"") && strstr(out.ptr, "\"has_password\":false"), "defaults: %s", out.ptr);
    CHECKN(call("/api/net/save", "{\"host\":\"-oProxyCommand=x\",\"user\":\"root\",\"password\":\"pw\"}", 0, &out) == 400 && strstr(out.ptr, "bad_host"), "host starting with - refused");
    CHECKN(call("/api/net/save", "{\"host\":\"1.2.3.4;rm\",\"user\":\"root\",\"password\":\"pw\"}", 0, &out) == 400, "host with ; refused");
    CHECKN(call("/api/net/save", "{\"host\":\"1.2.3.4\",\"user\":\"-l\",\"password\":\"pw\"}", 0, &out) == 400 && strstr(out.ptr, "bad_user"), "user starting with - refused");
    CHECKN(call("/api/net/save", "{\"host\":\"1.2.3.4\",\"port\":\"70000\",\"user\":\"root\",\"password\":\"pw\"}", 0, &out) == 400 && strstr(out.ptr, "bad_port"), "port range");
    CHECKN(call("/api/net/save", "{\"host\":\"1.2.3.4\",\"user\":\"root\",\"password\":\"\"}", 0, &out) == 400 && strstr(out.ptr, "need_pass"), "first save needs a password");
    CHECKN(call("/api/net/save", "{\"host\":\"1.2.3.4\",\"user\":\"root\",\"password\":\"a\\nb\"}", 0, &out) == 400 && strstr(out.ptr, "bad_pass"), "newline in password refused");
    CHECKN(call("/api/net/tunnel", "{\"on\":true}", 0, &out) == 400 && strstr(out.ptr, "no_server"), "tunnel needs a server first");

    /* 存取: 密码不回传、文件 0600、留空 = 不改 */
    CHECKN(call("/api/net/save", "{\"host\":\"cloud.example.com\",\"port\":\"2222\",\"user\":\"ubuntu\",\"password\":\"s3cret pass!\"}", 0, &out) == 200
           && strstr(out.ptr, "\"has_password\":true") && strstr(out.ptr, "\"port\":2222") && !strstr(out.ptr, "s3cret"), "saved, password not echoed: %s", out.ptr);
    struct stat st; snprintf(p, sizeof p, "%s/cloud", dir);
    CHECKN(!stat(p, &st) && (st.st_mode & 0777) == 0600, "cloud file is 0600");
    tr_cloud c;
    CHECKN(tr_cloud_load(&c) && !strcmp(c.pass, "s3cret pass!") && strlen(c.token) == 64 && !c.tunnel, "round trip + token generated");
    char tok[65]; snprintf(tok, sizeof tok, "%s", c.token);
    { char *f = tr_slurp(p, NULL); CHECKN(f && strstr(f, "\next_token -\n"), "empty ext_token written as '-', not stack garbage"); free(f); }
    c.frp_port = 7001; snprintf(c.ext_token, sizeof c.ext_token, "t#k \"x\"");
    CHECKN(tr_cloud_save(&c) && tr_cloud_load(&c) && c.frp_port == 7001 && !strcmp(c.ext_token, "t#k \"x\""), "frp_port / ext_token round trip");
    c.frp_port = 0; c.ext_token[0] = 0; tr_cloud_save(&c);
    CHECKN(call("/api/net/save", "{\"host\":\"cloud.example.com\",\"port\":\"22\",\"user\":\"ubuntu\",\"password\":\"\"}", 0, &out) == 200
           && tr_cloud_load(&c) && !strcmp(c.pass, "s3cret pass!") && !strcmp(c.token, tok) && c.port == 22, "empty password keeps the old one, token stable");

    /* 公网端口: 空 = 默认(没证书时 = 页面端口), 7000 / 非数字拒, 设了就存下并生效 */
    CHECKN(call("/api/net/status", NULL, 0, &out) == 200 && strstr(out.ptr, "\"public\":18000"), "default public port = page port without cert: %s", out.ptr);
    CHECKN(call("/api/net/save", "{\"host\":\"cloud.example.com\",\"user\":\"ubuntu\",\"password\":\"\",\"pub\":\"7000\"}", 0, &out) == 400 && strstr(out.ptr, "pub_is_frp"), "pub 7000 refused");
    CHECKN(call("/api/net/save", "{\"host\":\"cloud.example.com\",\"user\":\"ubuntu\",\"password\":\"\",\"pub\":\"84x3\"}", 0, &out) == 400 && strstr(out.ptr, "bad_pub"), "pub non-numeric refused");
    CHECKN(call("/api/net/save", "{\"host\":\"cloud.example.com\",\"user\":\"ubuntu\",\"password\":\"\",\"pub\":\"8443\"}", 0, &out) == 200
           && strstr(out.ptr, "\"pub\":8443") && strstr(out.ptr, "\"public\":8443") && tr_cloud_load(&c) && c.pub == 8443, "pub saved: %s", out.ptr);
    CHECKN(call("/api/net/save", "{\"host\":\"cloud.example.com\",\"user\":\"ubuntu\",\"password\":\"\",\"pub\":\"\"}", 0, &out) == 200 && tr_cloud_load(&c) && !c.pub, "pub cleared");

    /* askpass: 按链接所在目录读密码(子进程里调, 收它的 stdout) */
    {
        int pf[2]; char got[256] = ""; snprintf(p, sizeof p, "%s/ds4-askpass", dir);
        fflush(stdout);   /* 不然子进程会把父进程没冲出去的输出一起写进管道 */
        if (!pipe(pf)) {
            const pid_t pid = fork();
            if (pid == 0) { dup2(pf[1], 1); close(pf[0]); _exit(tr_askpass_main(p, "ubuntu@cloud's password: ")); }
            close(pf[1]); const ssize_t n = read(pf[0], got, sizeof got - 1); close(pf[0]); if (n > 0) got[n] = 0;
            int ws = 0; waitpid(pid, &ws, 0);
            CHECKN(WIFEXITED(ws) && !WEXITSTATUS(ws) && !strcmp(got, "s3cret pass!\n"), "askpass prints the password: [%s]", got);
        }
        CHECKN(tr_askpass_main(p, "Are you sure you want to continue connecting (yes/no/[fingerprint])?") == 1, "askpass refuses host-key prompts");
    }

    /* 证书夹具: 自签 CA → 域名证书(带链 bundle + 只有叶证书的一份 + 一把不配对的私钥), 再来一张已过期的 */
    const char *g1[] = { "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "ca.key", "-out", "ca.crt", "-days", "3", "-subj", "/CN=t-ca", NULL };
    const char *g2[] = { "openssl", "req", "-newkey", "rsa:2048", "-nodes", "-keyout", "site.key", "-out", "site.csr", "-subj", "/CN=t.ds4.test", NULL };
    const char *g3[] = { "openssl", "x509", "-req", "-in", "site.csr", "-CA", "ca.crt", "-CAkey", "ca.key", "-CAcreateserial", "-out", "leaf.crt", "-days", "2", NULL };
    const char *g4[] = { "openssl", "genrsa", "-out", "other.key", "2048", NULL };
    const char *g5[] = { "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "old.key", "-out", "old.crt", "-subj", "/CN=old.ds4.test",
                         "-not_before", "20200101000000Z", "-not_after", "20200201000000Z", NULL };
    const char *g6[] = { "openssl", "pkey", "-in", "site.key", "-aes256", "-passout", "pass:x", "-out", "enc.key", NULL };
    const bool fx_ok = sh(fx, g1) && sh(fx, g2) && sh(fx, g3) && sh(fx, g4) && sh(fx, g6);
    CHECKN(fx_ok, "fixtures (needs openssl)");
    if (fx_ok) {
        char a[4400], b[4400];
        snprintf(a, sizeof a, "%s/leaf.crt", fx); snprintf(b, sizeof b, "%s/ca.crt", fx);
        size_t la = 0, lb = 0; char *A = tr_slurp(a, &la), *B = tr_slurp(b, &lb);
        snprintf(p, sizeof p, "%s/bundle.crt", fx); FILE *f = fopen(p, "wb"); if (f && A && B) { fwrite(A, 1, la, f); fwrite(B, 1, lb, f); } if (f) fclose(f);
        free(A); free(B);
        const char *z1[] = { "zip", "-qj", "good.zip", "leaf.crt", "bundle.crt", "site.key", "other.key", NULL };
        const char *z2[] = { "zip", "-qj", "bad.zip", "leaf.crt", "other.key", NULL };
        const char *z3[] = { "zip", "-qj", "enc.zip", "leaf.crt", "enc.key", NULL };
        CHECKN(sh(fx, z1) && sh(fx, z2) && sh(fx, z3), "zip fixtures (needs zip)");
        snprintf(p, sizeof p, "%s/bad.zip", fx);  CHECKN(upload(p, &out) == 400 && strstr(out.ptr, "mismatch"), "mismatched pair refused: %s", out.ptr);
        snprintf(p, sizeof p, "%s/enc.zip", fx);  CHECKN(upload(p, &out) == 400 && strstr(out.ptr, "key_encrypted"), "encrypted key refused: %s", out.ptr);
        CHECKN(call("/api/net/cert", "not a zip at all", 0, &out) == 400 && strstr(out.ptr, "not_zip"), "not a zip");
        snprintf(p, sizeof p, "%s/good.zip", fx); CHECKN(upload(p, &out) == 200 && strstr(out.ptr, "\"chain\":2") && strstr(out.ptr, "bundle.crt"), "bundle with chain picked: %s", out.ptr);
        char dom[256]; CHECKN(tr_cert_domain(dom, sizeof dom) && !strcmp(dom, "t.ds4.test"), "domain from CN: %s", dom);
        snprintf(p, sizeof p, "%s/tls/key.pem", dir); CHECKN(!stat(p, &st) && (st.st_mode & 0777) == 0600, "key.pem is 0600");
        if (sh(fx, g5)) {   /* -not_after 要 OpenSSL 3; LibreSSL 没有就跳过这一条 */
            const char *z4[] = { "zip", "-qj", "old.zip", "old.crt", "old.key", NULL };
            snprintf(p, sizeof p, "%s/old.zip", fx); CHECKN(sh(fx, z4) && upload(p, &out) == 400 && strstr(out.ptr, "expired"), "expired refused: %s", out.ptr);
        } else printf("SKIP expired-cert check (openssl without -not_after)\n");
        CHECKN(call("/api/net/cert_delete", "{}", 0, &out) == 200 && strstr(out.ptr, "\"cert\":null"), "cert deleted");
    }
    ds4_buf_free(&out);
    printf("studio net: %d passed, %d failed\n", passes, fails);
    return fails;
}
