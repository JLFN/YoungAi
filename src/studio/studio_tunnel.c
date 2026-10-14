/* studio_tunnel.c — 开穿透的后台线程。总述见 studio_net.c。一次一条线程, 页面每秒轮询 /api/net/status 看走到哪一步。
 *
 * 部署(deploy = true, 页面打开开关 / 换了服务器):
 *   ssh   连上云服务器: 架构、是不是 root、有没有 systemd、装没装过同版本 frps; 再以 root 看一遍谁在监听哪个口、有没有别的 frps(studio_frps.c)。
 *         密码经 askpass 给(studio_exec.c); 不是 root 就 sudo -S, 同一个密码从 stdin 喂。
 *   定方案(10-12 用户的服务器实撞: 另一个项目的 frps 已经占着 7000; 用户: "只填 IP 和端口, 穿透能力就要考虑各种情况"):
 *         a) 我们装过的 ds4-frps 还在 → 接着用它;
 *         b) 有别的 frps, 读得到配置、是令牌认证、放行我们的公网端口 → 复用它, 服务器上什么都不装;
 *         c) 否则装我们自己的 ds4-frps: 7000 空着就用 7000, 被占就从 7001 往上找空口(控制口走 SSH, 换口不用动安全组)。
 *         公网端口被别的程序占着 → 直接报是谁占的(常见: nginx 占 443)。
 *   下载  本机 frpc + (方案 c)服务器架构的 frps(studio_frp.c, 有缓存)。
 *   上传  不用 scp: 新版 scp 默认走 SFTP, 有的服务器没开 sftp-server; 改成 ssh 'cat > 文件' 从 stdin 灌。frps 二进制只在版本不对时传。
 *   frps  装进 /usr/local/bin/ds4-frps + /etc/ds4-frp/frps.toml + systemd 单元 ds4-frps.service; ufw / firewalld 开着就顺手放行。
 *         只许 frpc 占公网端口(allowPorts; 没设公网端口时是 443 和页面端口两个, 换证书不用重新部署): 令牌万一泄露, 别人也开不了别的口。
 *   通道  拉起 ssh -L 控制通道(studio_sshtun.c): frps 控制口只听服务器本机, 安全组不用为它开口(10-12 用户定)。
 * 之后(deploy = false 只走这段: 开机自启、换证书):
 *   frpc  起本机 frpc, 等日志出 "start proxy success"(最多 20 秒)。
 *   公网  真发一条 HTTP(S) 请求取 /manifest.webmanifest, 拿到 200 才算通 —— 只连 TCP 在 TUN 模式下永远通过, 安全组没放行也会报"已连通"。
 *         有证书再查域名解析到的是不是这台服务器(不对只提醒)。
 * 出错会怎样: 停在出错那一步, 页面显示错误码 + 原始输出的最后一行; 完整 ssh 输出在 ~/.ds4/studio/frp/deploy.log。 */
#include "studio_internal.h"
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TR_SSH_TIMEOUT 60            /* 一条 ssh 命令: 连接 10 秒 + 远端执行; 传 12 MB 的 frps 在 1 MB/s 的线路上也够 */
#define TR_SSH_TRIES 3               /* 只对"握手被服务器丢掉"重试: 公网 sshd 常被扫描爆破, 未登录连接多了 MaxStartups 会随机丢新连接(10-12 实撞, 隔几秒就好) */
#define TR_PROXY_WAIT_S 20           /* frpc 起来到代理注册成功: 正常 1 秒内, 跨境线路握手慢也就几秒 */
#define TR_FRP_PORT_SCAN 100         /* 7000 被占时往上找空口的范围 */
#define TR_LISTENERS 512
#define TR_SVC "ds4-frps"

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static tr_tun_phase g_phase;
static char g_step[16], g_err[512], g_warn[256], g_note[256];   /* note: 定方案的结果(复用了谁 / 换了哪个口), 一直显示 */
static unsigned g_gen;                /* 每开一次 +1: 老线程发现自己过期了就不再改状态、不再起 frpc */
static bool g_deploy;                 /* 这一趟走不走 ssh 部署(页面据此画几步) */

static bool set_step(unsigned gen, const char *step) {
    pthread_mutex_lock(&g_mu);
    const bool live = gen == g_gen;
    if (live) snprintf(g_step, sizeof g_step, "%s", step);
    pthread_mutex_unlock(&g_mu);
    return live;
}
static void set_text(unsigned gen, char *dst, size_t n, const char *v) {
    pthread_mutex_lock(&g_mu); if (gen == g_gen) snprintf(dst, n, "%s", v); pthread_mutex_unlock(&g_mu);
}
static void finish(unsigned gen, tr_tun_phase ph, const char *err) {
    pthread_mutex_lock(&g_mu);
    if (gen == g_gen) { g_phase = ph; snprintf(g_err, sizeof g_err, "%s", err ? err : ""); }
    pthread_mutex_unlock(&g_mu);
}

/* 部署日志 0600; 读别人 frps 配置(cat)的输出不落盘 —— 里面有它的令牌(10-12 spark 实撞: 令牌进了 0644 的日志) */
static void deploy_log(const char *what, int rc, const ds4_buf *o) {
    char p[TR_PATH + 32]; snprintf(p, sizeof p, "%s/frp", tr_net_dir()); mkdir(tr_net_dir(), 0700); mkdir(p, 0700);   /* 第一步 ssh 时 frp/ 还没建 */
    snprintf(p, sizeof p, "%s/frp/deploy.log", tr_net_dir());
    const int fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0600); if (fd < 0) return;
    fchmod(fd, 0600);   /* 老版本留下的 0644 文件也收紧 */
    FILE *f = fdopen(fd, "a"); if (!f) { close(fd); return; }
    tr_logf(f, "== %.300s%s (rc %d)", what, strlen(what) > 300 ? "…" : "", rc);
    if (strstr(what, "cat \"/proc/")) fprintf(f, "(%zu bytes, not logged: contains the frps token)\n", o->len);
    else if (o->len) fwrite(o->ptr, 1, o->len, f);
    fputc('\n', f); fclose(f);
}
static const char *tail(ds4_buf *o) {
    if (!o->len) return "";
    char *e = o->ptr + o->len;
    while (e > o->ptr && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
    char *s = e; while (s > o->ptr && s[-1] != '\n') s--;
    return s;
}

/* 远端跑一条命令(经服务器上用户的登录 shell 解释; 命令里只有我们拼的固定路径与数字, 以及过了字符检查的路径) */
static int ssh_run(const tr_cloud *c, const char *cmd, const char *in, size_t in_len, ds4_buf *o) {
    char port[16], kh[TR_PATH + 64], khopt[TR_PATH + 96];
    snprintf(port, sizeof port, "%d", c->port);
    snprintf(kh, sizeof kh, "%s/known_hosts", tr_net_dir()); snprintf(khopt, sizeof khopt, "UserKnownHostsFile=%s", kh);
    const char *argv[] = { "ssh", "-p", port, "-l", c->user, "-o", "StrictHostKeyChecking=accept-new", "-o", khopt,
        "-o", "ConnectTimeout=10", "-o", "PubkeyAuthentication=no", "-o", "PreferredAuthentications=password,keyboard-interactive",
        "-o", "NumberOfPasswordPrompts=1", "-o", "ServerAliveInterval=10", "-o", "LogLevel=ERROR", "--", c->host, cmd, NULL };
    int rc = 0;
    for (int a = 0; a < TR_SSH_TRIES; a++) {
        o->len = 0; if (o->ptr) o->ptr[0] = 0;
        rc = tr_exec(argv, in, in_len, o, TR_SSH_TIMEOUT, TR_EXEC_ASKPASS);
        deploy_log(cmd, rc, o);
        const bool dropped = rc == 255 && o->ptr && (strstr(o->ptr, "banner exchange") || strstr(o->ptr, "kex_exchange_identification") || strstr(o->ptr, "reset by peer"));
        if (!dropped) break;
        sleep(3);
    }
    return rc;
}
/* 以 root 跑: 不是 root 就 sudo -S(-p '' 不打提示), 密码从 stdin 喂。cmd 里不许有单引号(外面套 sh -c '…') */
static int root_run(const tr_cloud *c, int uid, const char *cmd, ds4_buf *o) {
    if (!uid) return ssh_run(c, cmd, NULL, 0, o);
    ds4_buf full = {0}; ds4_buf_printf(&full, "sudo -S -p '' sh -c '%s'", cmd);
    char pw[200]; const int pl = snprintf(pw, sizeof pw, "%s\n", c->pass);
    const int rc = ssh_run(c, full.ptr, pw, (size_t)pl, o);
    memset(pw, 0, sizeof pw); ds4_buf_free(&full);
    return rc;
}

/* ssh 自己失败(退出码 255)时按 stderr 归类成页面认得的错误码 */
static void ssh_err(int rc, ds4_buf *o, char *err, size_t en) {
    const char *t = tail(o);
    const char *code = rc == TR_EXEC_TIMEOUT ? "ssh_conn" : strstr(t, "Permission denied") ? "ssh_auth"
        : strstr(o->ptr ? o->ptr : "", "IDENTIFICATION HAS CHANGED") ? "ssh_hostkey"
        : strstr(t, "timed out") || strstr(t, "refused") || strstr(t, "No route") || strstr(t, "resolve") || strstr(t, "unreachable") || strstr(t, "reset") ? "ssh_conn" : "ssh_other";
    snprintf(err, en, "%s: %s", code, rc == TR_EXEC_TIMEOUT ? "timeout" : t);
}
static void cmd_err(int rc, int uid, ds4_buf *o, const char *code, char *err, size_t en) {
    if (rc == 255 || rc == TR_EXEC_TIMEOUT) ssh_err(rc, o, err, en);
    else snprintf(err, en, "%s: %s", uid && o->ptr && (strstr(o->ptr, "sudo") || strstr(o->ptr, "sudoers")) ? "sudo" : code, tail(o));
}

/* 试着复用一个已有的 frps(方案 b)。pid / cmdline 来自普查; 成功填 port/token。why = 不能复用的原因(给页面看) */
static bool try_reuse(const tr_cloud *c, int uid, int pid, const char *cmdline, const tr_listener *ls, int n, int pub,
                      int *port, char *addr, size_t adn, char *token, size_t tn, char *why, size_t wn) {
    char path[512], full[700], allow[512], cmd[800]; int bind = 0;
    ds4_buf o = {0};
    bool ok = false;
    if (!tr_frps_conf_path(cmdline, path, sizeof path)) { bind = TR_FRP_PORT; token[0] = 0; allow[0] = 0; ok = true; }   /* 没给 -c: 内置默认 */
    else if (strpbrk(path, "'\"$`\\")) snprintf(why, wn, "config path %.200s", path);
    else {
        /* 经 /proc/<pid>/root 读: frps 跑在 docker 里时, 命令行里的路径是容器里的, 从宿主直接读不到 */
        snprintf(full, sizeof full, path[0] == '/' ? "/proc/%d/root%s" : "/proc/%d/cwd/%s", pid, path);
        snprintf(cmd, sizeof cmd, "cat \"%s\"", full);
        const int rc = root_run(c, uid, cmd, &o);
        if (rc) snprintf(why, wn, "cannot read %.200s", path);
        else if (tr_frps_parse(o.ptr ? o.ptr : "", &bind, token, tn, allow, sizeof allow)) snprintf(why, wn, "%.200s: not token auth", path);
        else ok = true;
    }
    ds4_buf_free(&o);
    if (!ok) return false;
    const tr_listener *l = tr_listener_on(ls, n, bind);
    if (!l || l->pid != pid) { snprintf(why, wn, "frps pid %d not listening on its bindPort %d", pid, bind); return false; }
    if (!tr_ports_allow(allow, pub)) { snprintf(why, wn, "allowPorts %s excludes %d", allow, pub); return false; }
    *port = bind; snprintf(addr, adn, "%s", l->addr);   /* SSH 转发在服务器那头连它真正监听的地址 */
    return true;
}

/* 方案 c 的二进制 + 配置 + systemd */
static bool install_own(const tr_cloud *c, int uid, const char *have, const char *ga, int fport, int page_port, char *err, size_t en, unsigned gen) {
    char frps[TR_PATH + 64]; ds4_buf o = {0};
    if (!tr_frp_ensure("linux", ga, "frps", frps, sizeof frps, err, en)) return false;
    if (!set_step(gen, "upload")) return false;
    const bool need_bin = strcmp(have, TR_FRP_VER) != 0;
    int rc;
    if (need_bin) {
        size_t n = 0; char *bin = tr_slurp(frps, &n);
        rc = bin ? ssh_run(c, "mkdir -p .ds4-frp && cat > .ds4-frp/frps.part && mv .ds4-frp/frps.part .ds4-frp/frps", bin, n, &o) : -1;
        free(bin);
        if (rc) { cmd_err(rc, uid, &o, "upload", err, en); ds4_buf_free(&o); return false; }
    }
    /* 公网端口: 设了就只有它一个; 没设就 https / http 两个默认口都放(有没有证书随时会变, 换证书只重起 frpc, 不重新部署) */
    const int p1 = tr_pub_port(c, true, page_port), p2 = tr_pub_port(c, false, page_port);
    ds4_buf cfg = {0};
    ds4_buf_printf(&cfg, "# 由 ds4-train 生成(src/studio/studio_tunnel.c), 每次开穿透重写\nbindAddr = \"127.0.0.1\"\nbindPort = %d\n"
                         "proxyBindAddr = \"0.0.0.0\"\n"   /* 不写会跟着 bindAddr 走: 公网口也只听本机, 外面全连不上 */
                         "auth.token = \"%s\"\nallowPorts = [ { single = %d }, { single = %d } ]\n", fport, c->token, p1, p2);
    rc = ssh_run(c, "mkdir -p .ds4-frp && umask 077 && cat > .ds4-frp/frps.toml", cfg.ptr, cfg.len, &o);
    ds4_buf_free(&cfg);
    if (rc) { cmd_err(rc, uid, &o, "upload", err, en); ds4_buf_free(&o); return false; }
    if (!set_step(gen, "frps")) { ds4_buf_free(&o); return false; }
    char cmd[3072], fw[640], unit[512];
    snprintf(unit, sizeof unit, "[Unit]\\nDescription=ds4 studio frp server\\nAfter=network-online.target\\nWants=network-online.target\\n\\n"
             "[Service]\\nExecStart=/usr/local/bin/" TR_SVC " -c /etc/ds4-frp/frps.toml\\nRestart=always\\nRestartSec=3\\n\\n[Install]\\nWantedBy=multi-user.target\\n");
    snprintf(fw, sizeof fw, "( if command -v ufw >/dev/null && ufw status | grep -q \"Status: active\"; then ufw allow %d/tcp; ufw allow %d/tcp; fi >/dev/null; "
             "if command -v firewall-cmd >/dev/null && firewall-cmd --state >/dev/null 2>&1; then firewall-cmd -q --permanent --add-port=%d/tcp --add-port=%d/tcp; firewall-cmd -q --reload; fi ) || true; ",
             p1, p2, p1, p2);   /* 控制口只听本机, 不放行 */
    /* 失败时把 journal 尾巴带出来, 页面能看到 frps 为什么起不来 */
    snprintf(cmd, sizeof cmd, "set -e; cd ~%s/.ds4-frp; %s mkdir -p /etc/ds4-frp; install -m 600 frps.toml /etc/ds4-frp/frps.toml; "
             "printf \"%s\" > /etc/systemd/system/" TR_SVC ".service; systemctl daemon-reload; systemctl enable " TR_SVC " >/dev/null 2>&1; "
             "%s systemctl restart " TR_SVC "; sleep 1; systemctl is-active " TR_SVC " || { journalctl -u " TR_SVC " -n 8 --no-pager; exit 1; }; rm -f frps",
             c->user, need_bin ? "install -m 755 frps /usr/local/bin/" TR_SVC ";" : "", unit, fw);
    rc = root_run(c, uid, cmd, &o);
    if (rc) { cmd_err(rc, uid, &o, "frps_start", err, en); ds4_buf_free(&o); return false; }
    ds4_buf_free(&o);
    return true;
}

static bool deploy_with(unsigned gen, tr_cloud *c, int page_port, char *err, size_t en, tr_listener *ls) {
    ds4_buf o = {0};
    tr_frpc_stop(); tr_sshfwd_stop();   /* 先撤掉我们自己的旧代理与旧通道: 不然普查时公网口还被它占着, 会误判成冲突; 换了服务器旧通道也不能用 */
    if (!set_step(gen, "ssh")) return false;
    int rc = ssh_run(c, "echo ds4-arch=$(uname -m) ds4-uid=$(id -u) ds4-systemd=$(command -v systemctl >/dev/null && echo 1 || echo 0) "
                        "ds4-have=$(/usr/local/bin/" TR_SVC " -v 2>/dev/null || echo none)", NULL, 0, &o);
    char arch[32] = "", have[32] = ""; int uid = -1, sysd = 0;
    const char *m = o.ptr ? strstr(o.ptr, "ds4-arch=") : NULL;
    if (rc || !m || sscanf(m, "ds4-arch=%31s ds4-uid=%d ds4-systemd=%d ds4-have=%31s", arch, &uid, &sysd, have) != 4) {
        if (rc == 255 || rc == TR_EXEC_TIMEOUT) ssh_err(rc, &o, err, en); else snprintf(err, en, "ssh_other: rc %d %s", rc, tail(&o));
        ds4_buf_free(&o); return false;
    }
    const char *ga = !strcmp(arch, "x86_64") ? "amd64" : !strcmp(arch, "aarch64") || !strcmp(arch, "arm64") ? "arm64" : NULL;
    if (!ga) { snprintf(err, en, "arch: %s", arch); ds4_buf_free(&o); return false; }
    /* 普查(要 root 才看得到别人进程的名字): 谁在监听哪个口 + 所有 frps 进程的命令行 */
    rc = root_run(c, uid, "ss -ltnpH; echo ds4-frps-list; for p in $(pgrep -x frps); do echo \"ds4-frps $p $(tr \"\\0\" \" \" < /proc/$p/cmdline)\"; done", &o);
    if (rc) { cmd_err(rc, uid, &o, "survey", err, en); ds4_buf_free(&o); return false; }
    const int n = tr_listeners_parse(o.ptr, ls, TR_LISTENERS);
    char crt[TR_PATH], key[TR_PATH]; const bool tls = tr_cert_present(crt, sizeof crt, key, sizeof key);
    const int pub = tr_pub_port(c, tls, page_port);
    int fport = 0, rpid = 0; char etok[200] = "", why[300] = "", note[256] = "", faddr[64] = "";
    for (int i = 0; i < n; i++)   /* a) 我们的 ds4-frps 还在: 优先上次记下的口 */
        if (!strcmp(ls[i].name, TR_SVC) && ls[i].port != pub && (!fport || ls[i].port == c->frp_port)) fport = ls[i].port;
    if (!fport) {   /* b) 别的 frps */
        char *list = o.ptr ? strstr(o.ptr, "ds4-frps-list") : NULL;
        for (char *ln = list ? strstr(list, "\nds4-frps ") : NULL; ln && !fport; ln = strstr(ln + 1, "\nds4-frps ")) {
            int pid = 0, off = 0; char cl[1024];
            if (sscanf(ln + 1, "ds4-frps %d %n", &pid, &off) < 1) continue;
            snprintf(cl, sizeof cl, "%.*s", (int)strcspn(ln + 1 + off, "\n"), ln + 1 + off);
            if (try_reuse(c, uid, pid, cl, ls, n, pub, &fport, faddr, sizeof faddr, etok, sizeof etok, why, sizeof why)) { rpid = pid; snprintf(note, sizeof note, "reuse_frps: %d (pid %d)", fport, pid); }
            else fport = 0;
        }
    }
    if (!fport) {   /* c) 自己装: 7000 空着就用, 否则往上找 */
        for (int p = TR_FRP_PORT; p < TR_FRP_PORT + TR_FRP_PORT_SCAN && !fport; p++) if (p != pub && p != page_port && !tr_listener_on(ls, n, p)) fport = p;
        if (!fport) { snprintf(err, en, "frp_port_none: %d-%d", TR_FRP_PORT, TR_FRP_PORT + TR_FRP_PORT_SCAN - 1); ds4_buf_free(&o); return false; }
        const tr_listener *h = tr_listener_on(ls, n, TR_FRP_PORT);
        if (fport != TR_FRP_PORT) snprintf(note, sizeof note, "frp_port_moved: %d (%d %s%s%s)", fport, TR_FRP_PORT, h ? h->name : "?", why[0] ? "; " : "", why);
        else if (why[0]) snprintf(note, sizeof note, "reuse_skipped: %s", why);
    }
    const tr_listener *hp = tr_listener_on(ls, n, pub);
    if (hp && strcmp(hp->name, TR_SVC) && !(rpid && hp->pid == rpid)) {
        snprintf(err, en, "pub_busy: %d %s (pid %d)", pub, hp->name[0] ? hp->name : "?", hp->pid); ds4_buf_free(&o); return false;
    }
    ds4_buf_free(&o);
    set_text(gen, g_note, sizeof g_note, note);
    /* 认出来的方案写进 cloud 文件: 开机自启只起 frpc, 要知道连哪个口、用谁的令牌 */
    c->frp_port = fport; snprintf(c->ext_token, sizeof c->ext_token, "%s", rpid ? etok : ""); snprintf(c->frp_bind, sizeof c->frp_bind, "%s", rpid && strcmp(faddr, "127.0.0.1") ? faddr : "");
    tr_cloud cur;
    if (tr_cloud_load(&cur) && !strcmp(cur.host, c->host)) {
        cur.frp_port = c->frp_port; snprintf(cur.ext_token, sizeof cur.ext_token, "%s", c->ext_token); snprintf(cur.frp_bind, sizeof cur.frp_bind, "%s", c->frp_bind); tr_cloud_save(&cur);
    }

    if (!set_step(gen, "download")) return false;
    char frpc[TR_PATH + 64];   /* 本机 frpc 在这一步下好: 下载失败就停在"下载", 不要等服务器那头都弄完了才发现 */
    if (!tr_frpc_ensure(frpc, sizeof frpc, err, en)) return false;
    if (!rpid && (!sysd ? (snprintf(err, en, "no_systemd"), false) : !install_own(c, uid, have, ga, fport, page_port, err, en, gen))) return false;
    if (!set_step(gen, "sshfwd")) return false;   /* 控制通道: 这里先拉起来, 认证/转发的错在这一步报; 下一步 frpc 直接用它 */
    return tr_sshfwd_start(c, c->frp_bind, fport, err, en) > 0;
}

static bool deploy(unsigned gen, tr_cloud *c, int page_port, char *err, size_t en) {
    tr_listener *ls = calloc(TR_LISTENERS, sizeof *ls);   /* 每趟一张: 快速开关时老线程可能还没跑完, 不能共用 */
    if (!ls) { snprintf(err, en, "thread: oom"); return false; }
    const bool ok = deploy_with(gen, c, page_port, err, en, ls);
    free(ls);
    return ok;
}

/* 真发一条请求: 拿到 200 才算公网通(只连 TCP 在本机 TUN 代理下永远成功)。https 不验证书(上传时验过了, 这里只看路通不通) */
static bool http_probe(const tr_cloud *c, bool tls, int pub, const char *dom, char *got, size_t gn) {
    char url[512], conn[600]; ds4_buf o = {0};
    const char *h6 = strchr(c->host, ':') ? "[" : "", *h6e = strchr(c->host, ':') ? "]" : "";
    if (tls && dom[0]) { snprintf(url, sizeof url, "https://%s:%d/manifest.webmanifest", dom, pub); snprintf(conn, sizeof conn, "%s:%d:%s%s%s:%d", dom, pub, h6, c->host, h6e, pub); }
    else snprintf(url, sizeof url, "%s://%s%s%s:%d/manifest.webmanifest", tls ? "https" : "http", h6, c->host, h6e, pub);
    const char *a1[] = { "curl", "-sk", "-o", "/dev/null", "-w", "%{http_code}", "--noproxy", "*", "-m", "12", "--connect-to", conn, url, NULL };
    const char *a2[] = { "curl", "-sk", "-o", "/dev/null", "-w", "%{http_code}", "--noproxy", "*", "-m", "12", url, NULL };
    tr_exec(tls && dom[0] ? a1 : a2, NULL, 0, &o, 20, TR_EXEC_NOPROXY);
    snprintf(got, gn, "%s", o.ptr ? o.ptr : "");
    ds4_buf_free(&o);
    return !strcmp(got, "200");
}

typedef struct { unsigned gen; int page_port; bool deploy; } job;

static void *run(void *a) {
    const job j = *(job *)a; free(a);
    tr_cloud c; char err[512] = "", line[400];
    if (!tr_cloud_load(&c) || !c.host[0] || !c.token[0]) { finish(j.gen, TR_TUN_ERROR, "no_server"); goto out; }
    if (j.deploy && !deploy(j.gen, &c, j.page_port, err, sizeof err)) { finish(j.gen, TR_TUN_ERROR, err); goto out; }
    if (!set_step(j.gen, "frpc")) goto out;
    if (!tr_frpc_start(&c, j.page_port, err, sizeof err)) { finish(j.gen, TR_TUN_ERROR, err); goto out; }
    pthread_mutex_lock(&g_mu); const bool stale_off = j.gen != g_gen && g_phase == TR_TUN_OFF; pthread_mutex_unlock(&g_mu);
    if (stale_off) { tr_frpc_stop(); goto out; }   /* 起 frpc 的同时用户关了穿透: 刚起的这个不能留 */
    int st = 1, ex = -1;
    for (int i = 0; i < TR_PROXY_WAIT_S * 2 && (st = tr_frpc_state(line, sizeof line, &ex)) == 1; i++) usleep(500000);
    if (st != 2) {   /* 令牌不对 / 端口被占 / 连不上 frps: 日志最后一条就是原因 */
        char fl[300]; const bool fup = tr_sshfwd_state(fl, sizeof fl);
        if (strstr(fl, "prohibited")) snprintf(err, sizeof err, "ssh_nofwd: %s", fl);   /* sshd 关了端口转发 */
        else if (strstr(fl, "open failed")) snprintf(err, sizeof err, "fwd_open: %s", fl);   /* 服务器那头没人听控制口(frps 没起来) */
        else if (!fup) snprintf(err, sizeof err, "sshfwd: %s", fl[0] ? fl : "ssh exited");
        else {
            const char *code = st == 0 ? "frpc_exit" : strstr(line, "token") ? "frpc_login" : strstr(line, "port") ? "proxy" : "frpc_conn";
            snprintf(err, sizeof err, "%s: %s", code, line[0] ? line : "no log");
        }
        finish(j.gen, TR_TUN_ERROR, err); goto out;
    }
    if (!set_step(j.gen, "public")) goto out;
    char crt[TR_PATH], key[TR_PATH], dom[256] = "";
    const bool tls = tr_cert_present(crt, sizeof crt, key, sizeof key);
    const int pub = tr_pub_port(&c, tls, j.page_port);
    if (tls) tr_cert_domain(dom, sizeof dom);
    char got[32];
    if (!http_probe(&c, tls, pub, dom, got, sizeof got)) { snprintf(err, sizeof err, "port_public: %s:%d (HTTP %s)", c.host, pub, got[0] ? got : "000"); finish(j.gen, TR_TUN_ERROR, err); goto out; }
    if (tls && dom[0]) {   /* 域名要解析到云服务器, 浏览器才会连到它; 对不上只提醒 */
        char a1[256], a2[256]; tr_resolve4(dom, a1, sizeof a1); tr_resolve4(c.host, a2, sizeof a2);
        pthread_mutex_lock(&g_mu);
        if (j.gen == g_gen) {
            if (!a1[0]) snprintf(g_warn, sizeof g_warn, "dns_none: %s", dom);
            else if (!a2[0] || !strstr(a1, a2)) snprintf(g_warn, sizeof g_warn, "dns_mismatch: %.80s → %.70s ≠ %.70s", dom, a1, a2[0] ? a2 : c.host);   /* 显示用, 截断无妨 */
        }
        pthread_mutex_unlock(&g_mu);
    }
    finish(j.gen, TR_TUN_UP, NULL);
out:
    return NULL;
}

void tr_tunnel_start(int page_port, bool deploy_it) {
    job *j = malloc(sizeof *j);
    pthread_mutex_lock(&g_mu);
    j->gen = ++g_gen; j->page_port = page_port; j->deploy = deploy_it;
    g_phase = TR_TUN_BUSY; g_deploy = deploy_it; g_step[0] = g_err[0] = g_warn[0] = 0;
    if (deploy_it) g_note[0] = 0;   /* 只起 frpc 的那趟沿用上次部署认出来的方案说明 */
    pthread_mutex_unlock(&g_mu);
    pthread_t th;
    if (pthread_create(&th, NULL, run, j)) { free(j); finish(g_gen, TR_TUN_ERROR, "thread"); return; }
    pthread_detach(th);
}

void tr_tunnel_stop(void) {
    pthread_mutex_lock(&g_mu); g_gen++; g_phase = TR_TUN_OFF; g_step[0] = g_err[0] = g_warn[0] = g_note[0] = 0; pthread_mutex_unlock(&g_mu);
    tr_frpc_stop(); tr_sshfwd_stop();
}

void tr_tunnel_json(ds4_buf *out, const tr_cloud *c, int page_port) {
    pthread_mutex_lock(&g_mu);
    tr_tun_phase ph = g_phase; const bool dep = g_deploy; char step[16], err[512], warn[256], note[256];
    snprintf(step, sizeof step, "%s", g_step); snprintf(err, sizeof err, "%s", g_err); snprintf(warn, sizeof warn, "%s", g_warn); snprintf(note, sizeof note, "%s", g_note);
    pthread_mutex_unlock(&g_mu);
    char line[400]; int ex = -1;
    const int st = tr_frpc_state(line, sizeof line, &ex);
    /* 部署完以后的活状态按 frpc 自己判: 断网时 frpc 在重连(日志最后是警告), 进程没了就是挂了 */
    if (ph == TR_TUN_UP && st == 0) { ph = TR_TUN_ERROR; snprintf(err, sizeof err, "frpc_exit: %s (exit %d)", line[0] ? line : "?", ex); }
    const bool reconnecting = ph == TR_TUN_UP && st != 2;
    char crt[TR_PATH], key[TR_PATH], dom[256] = "", url[400] = "";
    const bool tls = tr_cert_present(crt, sizeof crt, key, sizeof key);
    const int pub = tr_pub_port(c, tls, page_port);
    if (c->host[0]) {
        if (tls) { if (tr_cert_domain(dom, sizeof dom)) { if (pub == TR_HTTPS_PORT) snprintf(url, sizeof url, "https://%s/", dom); else snprintf(url, sizeof url, "https://%s:%d/", dom, pub); } }
        else snprintf(url, sizeof url, strchr(c->host, ':') ? "http://[%s]:%d/" : "http://%s:%d/", c->host, pub);
    }
    static const char *const names[] = { "off", "busy", "up", "error" };
    ds4_buf_printf(out, "\"tunnel\":{\"on\":%s,\"phase\":\"%s\",\"reconnecting\":%s,\"step\":\"%s\",\"deploy\":%s,\"tls\":%s,\"error\":",
                   c->tunnel ? "true" : "false", names[ph], reconnecting ? "true" : "false", step, dep ? "true" : "false", tls ? "true" : "false");
    ds4_json_escape(out, err); ds4_buf_puts(out, ",\"warn\":"); ds4_json_escape(out, warn); ds4_buf_puts(out, ",\"note\":"); ds4_json_escape(out, note);
    char fl[300]; const bool fup = tr_sshfwd_state(fl, sizeof fl);
    ds4_buf_printf(out, ",\"fwd_up\":%s,\"fwd_log\":", fup ? "true" : "false"); ds4_json_escape(out, fl);
    ds4_buf_puts(out, ",\"log\":"); ds4_json_escape(out, line); ds4_buf_puts(out, ",\"url\":"); ds4_json_escape(out, url); ds4_buf_putc(out, '}');
}
