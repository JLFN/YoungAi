/* studio_frp.c — frp 二进制与本机 frpc 子进程。总述见 studio_net.c。
 * 二进制: 第一次开穿透时从 GitHub 发布页下(本机要 frpc, 云服务器要对应架构的 frps), 用同一发布页的 frp_sha256_checksums.txt 校验,
 *   对不上就删掉报错 —— frps 是要以 root 跑在你云服务器上的, 不校验等于把服务器交给下载链路上的任何人。
 *   缓存在 ~/.ds4/studio/frp/<版本>/<系统>_<架构>/, 下过一次不再下。下不动(国内直连 GitHub 慢、主进程没带代理)就把官方包里的
 *   frpc / frps 手动放进这个目录, 有了就不下(手放的不校验, 自己负责来源)。
 * frpc: 自成会话的子进程, pid 记在 frp/frpc.pid —— 主进程被 kill -9 后 frpc 还活着, 下次起来先按 pid 文件杀掉它, 不然两个 frpc 抢同一个公网口
 *   (frps 那边报 "port already used")。loginFailExit = false: 断网、云服务器重启时 frpc 自己一直重连, 不退出。
 *   日志不用 frp 自己的 log.to, 而是 stdout 重定向进 frp/frpc.log: 配置写错时 frpc 的报错只打在 stdout, log.to 里什么都没有。 */
#include "studio_internal.h"
#include "../common/ds4_sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#endif

#define TR_FRP_URL "https://github.com/fatedier/frp/releases/download/v" TR_FRP_VER "/"

#if defined(__aarch64__) || defined(__arm64__)
#define TR_ARCH "arm64"
#elif defined(__x86_64__)
#define TR_ARCH "amd64"
#else
#define TR_ARCH "unsupported"
#endif
#ifdef __APPLE__
#define TR_OS "darwin"
#else
#define TR_OS "linux"
#endif

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pid_t g_pid;
static int g_exit = -1;   /* 上一个 frpc 的退出码(-1 = 没退过) */
/* 日志读到哪儿 + 读出来的连接状态(tr_frpc_state; 每次起 frpc 清零)。不再每次截日志尾巴重猜: 公网口上的 TLS 握手报错每分钟好几条,
   起来一小时尾巴就被灌满, "start proxy success" 滑出窗口, 页面一直显示断线(10-12 spark 实撞, 日志 38 KB 而窗口 32 KB) */
static off_t g_log_off;
static int g_conn = 1;
static char g_conn_line[400], g_last_line[400];   /* 定下当前状态的那行 / 最后一行非空日志(进程退了, 它就是死因) */

static void mkdirs(const char *p) {
    char b[TR_PATH * 2]; snprintf(b, sizeof b, "%s", p);
    for (char *s = b + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(b, 0700); *s = '/'; }
    mkdir(b, 0700);
}
static const char *last_line(ds4_buf *o) {   /* 输出的最后一个非空行(curl/tar 的报错就在这) */
    if (!o->len) return "";
    char *e = o->ptr + o->len;
    while (e > o->ptr && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
    char *s = e; while (s > o->ptr && s[-1] != '\n') s--;
    return s;
}

static bool download(const char *url, const char *dst, char *err, size_t en) {
    const char *argv[] = { "curl", "-fsSL", "--connect-timeout", "20", "--max-time", "900", "-o", dst, url, NULL };
    ds4_buf o = {0};
    const int rc = tr_exec(argv, NULL, 0, &o, 960, 0);
    if (rc) snprintf(err, en, "download: %s (%s)", rc == TR_EXEC_TIMEOUT ? "timeout" : last_line(&o), url);
    ds4_buf_free(&o);
    if (rc) unlink(dst);
    return rc == 0;
}

static bool sha256_file(const char *path, char hex[65]) {
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    ds4_sha256_ctx c; ds4_sha256_init(&c);
    char buf[65536]; ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) ds4_sha256_update(&c, buf, (size_t)n);
    close(fd);
    uint8_t h[32]; ds4_sha256_final(&c, h); tr_hex(h, 32, hex);
    return n == 0;
}

bool tr_frp_ensure(const char *os, const char *arch, const char *bin, char *path, size_t pn, char *err, size_t en) {
    char dir[TR_PATH + 64], sums[TR_PATH + 64], pkg[128], tgz[TR_PATH + 200], url[400];
    snprintf(dir, sizeof dir, "%s/frp/" TR_FRP_VER "/%s_%s", tr_net_dir(), os, arch);
    snprintf(path, pn, "%s/%s", dir, bin);
    if (!access(path, X_OK)) return true;
    if (!strcmp(arch, "unsupported")) { snprintf(err, en, "arch: %s", os); return false; }
    mkdirs(dir);
    snprintf(sums, sizeof sums, "%s/frp/" TR_FRP_VER "/frp_sha256_checksums.txt", tr_net_dir());
    if (access(sums, R_OK) && !download(TR_FRP_URL "frp_sha256_checksums.txt", sums, err, en)) return false;
    snprintf(pkg, sizeof pkg, "frp_" TR_FRP_VER "_%s_%s", os, arch);
    snprintf(tgz, sizeof tgz, "%s/%s.tar.gz", dir, pkg);
    snprintf(url, sizeof url, TR_FRP_URL "%s.tar.gz", pkg);
    if (!download(url, tgz, err, en)) return false;
    char got[65], want[65] = "", *s = tr_slurp(sums, NULL), needle[160];
    snprintf(needle, sizeof needle, "  %s.tar.gz", pkg);
    const char *hit = s ? strstr(s, needle) : NULL;
    if (hit && hit - s >= 64) snprintf(want, sizeof want, "%.64s", hit - 64);
    free(s);
    if (!sha256_file(tgz, got) || !want[0] || strcmp(got, want)) {
        unlink(tgz); unlink(sums);   /* 校验文件也删: 万一是它下坏了, 下次两样一起重下 */
        snprintf(err, en, "checksum: %s.tar.gz sha256 %s, want %s", pkg, got, want[0] ? want : "(not listed)");
        return false;
    }
    char m1[200], m2[200]; snprintf(m1, sizeof m1, "%s/frpc", pkg); snprintf(m2, sizeof m2, "%s/frps", pkg);
    const char *tv[] = { "tar", "xzf", tgz, "-C", dir, "--strip-components=1", m1, m2, NULL };
    ds4_buf o = {0};
    const int rc = tr_exec(tv, NULL, 0, &o, 120, 0);
    if (rc) snprintf(err, en, "extract: %s", last_line(&o));
    ds4_buf_free(&o); unlink(tgz);
    if (rc) return false;
    char other[TR_PATH + 80]; snprintf(other, sizeof other, "%s/%s", dir, strcmp(bin, "frpc") ? "frpc" : "frps");
    chmod(path, 0755); chmod(other, 0755);
    if (access(path, X_OK)) { snprintf(err, en, "extract: %s missing after tar", path); return false; }
    return true;
}

/* ---------------- frpc 子进程 ---------------- */
static void frp_path(char *out, size_t n, const char *leaf) { snprintf(out, n, "%s/frp/%s", tr_net_dir(), leaf); }

static void *reaper(void *a) {
    const pid_t p = (pid_t)(intptr_t)a;
    int st = 0;
    while (waitpid(p, &st, 0) < 0 && errno == EINTR) {}
    pthread_mutex_lock(&g_mu);
    if (g_pid == p) { g_pid = 0; g_exit = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st); }
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

/* pid 文件里那个进程还是 frpc 吗(pid 会被别的进程复用, 只按程序名认, 不能见 pid 就杀) */
static bool is_frpc(pid_t p) {
    char exe[TR_PATH] = "";
#ifdef __APPLE__
    if (proc_pidpath(p, exe, sizeof exe) <= 0) return false;
#else
    char link[64]; snprintf(link, sizeof link, "/proc/%d/exe", (int)p);
    const ssize_t n = readlink(link, exe, sizeof exe - 1); if (n <= 0) return false; exe[n] = 0;
#endif
    const char *b = strrchr(exe, '/');
    return !strcmp(b ? b + 1 : exe, "frpc");
}

static void kill_wait(pid_t p) {
    kill(p, SIGTERM);
    for (int i = 0; i < 30 && !kill(p, 0); i++) usleep(100000);
    if (!kill(p, 0)) kill(p, SIGKILL);
}

void tr_frpc_stop(void) {
    pthread_mutex_lock(&g_mu);
    const pid_t p = g_pid; g_pid = 0; g_exit = -1;
    pthread_mutex_unlock(&g_mu);
    if (p > 0) kill_wait(p);   /* 收尸归 reaper 线程 */
    char pf[TR_PATH + 32]; frp_path(pf, sizeof pf, "frpc.pid");
    char *s = tr_slurp(pf, NULL);
    if (s) { const pid_t old = (pid_t)atoi(s); free(s); if (old > 0 && old != p && is_frpc(old)) kill_wait(old); }
    unlink(pf);
}

static void toml_str(ds4_buf *b, const char *s) {   /* TOML 基本字符串: 只有 " 与 \ 要转义(路径来自家目录, 理论上什么字符都可能有) */
    ds4_buf_putc(b, '"');
    for (; *s; s++) { if (*s == '"' || *s == '\\') ds4_buf_putc(b, '\\'); ds4_buf_putc(b, *s); }
    ds4_buf_putc(b, '"');
}

bool tr_frpc_ensure(char *path, size_t pn, char *err, size_t en) { return tr_frp_ensure(TR_OS, TR_ARCH, "frpc", path, pn, err, en); }

bool tr_frpc_start(const tr_cloud *c, int page_port, char *err, size_t en) {
    char bin[TR_PATH + 64];
    if (!tr_frpc_ensure(bin, sizeof bin, err, en)) return false;
    tr_frpc_stop();
    int lport = tr_sshfwd_port();   /* 控制通道: 本机 ssh -L 那一头; 没起(开机自启、上一条被关)就现在拉 */
    if (!lport && !(lport = tr_sshfwd_start(c, c->frp_bind, tr_frp_port(c), err, en))) return false;
    char crt[TR_PATH + 32], key[TR_PATH + 32], conf[TR_PATH + 32], logp[TR_PATH + 32], pf[TR_PATH + 32];
    const bool tls = tr_cert_present(crt, sizeof crt, key, sizeof key);
    ds4_buf b = {0};
    ds4_buf_puts(&b, "# 由 ds4-train 生成(src/studio/studio_frp.c), 每次开穿透/换证书重写, 手改会被覆盖\nserverAddr = \"127.0.0.1\"");
    ds4_buf_printf(&b, "\nserverPort = %d\nauth.token = ", lport); toml_str(&b, tr_frp_token(c));   /* 复用别人的 frps 时令牌是它的, 什么字符都可能有 */
    ds4_buf_puts(&b, "\nloginFailExit = false\nlog.to = \"console\"\nlog.level = \"info\"\n\n");
    ds4_buf_printf(&b, "[[proxies]]\nname = \"ds4-studio\"\ntype = \"tcp\"\nremotePort = %d\n", tr_pub_port(c, tls, page_port));
    if (tls) {   /* frpc 在本机解 TLS 再以 http 转给页面端口(实测 tcp 代理 + https2http 插件可用, 10-12) */
        ds4_buf_printf(&b, "\n[proxies.plugin]\ntype = \"https2http\"\nlocalAddr = \"127.0.0.1:%d\"\ncrtPath = ", page_port);
        toml_str(&b, crt); ds4_buf_puts(&b, "\nkeyPath = "); toml_str(&b, key); ds4_buf_putc(&b, '\n');
    } else ds4_buf_printf(&b, "localIP = \"127.0.0.1\"\nlocalPort = %d\n", page_port);
    frp_path(conf, sizeof conf, "frpc.toml"); frp_path(logp, sizeof logp, "frpc.log"); frp_path(pf, sizeof pf, "frpc.pid");
    const int fd = open(conf, O_WRONLY | O_CREAT | O_TRUNC, 0600);   /* 有令牌: 0600 */
    const bool wok = fd >= 0 && write(fd, b.ptr, b.len) == (ssize_t)b.len;
    if (fd >= 0) close(fd);
    ds4_buf_free(&b);
    if (!wok) { snprintf(err, en, "frpc_start: cannot write %s: %s", conf, strerror(errno)); return false; }
    const int lf = open(logp, O_WRONLY | O_CREAT | O_TRUNC, 0600); if (lf >= 0) close(lf);   /* 只留这一次的日志, 状态才按它判 */
    const char *argv[] = { bin, "-c", conf, NULL };
    const pid_t p = tr_spawn_bg(argv, logp, TR_EXEC_NOPROXY);
    if (p < 0) { snprintf(err, en, "frpc_start: fork: %s", strerror(errno)); return false; }
    pthread_mutex_lock(&g_mu); g_pid = p; g_exit = -1; g_log_off = 0; g_conn = 1; g_conn_line[0] = g_last_line[0] = 0; pthread_mutex_unlock(&g_mu);
    pthread_t th; pthread_create(&th, NULL, reaper, (void *)(intptr_t)p); pthread_detach(th);
    FILE *f = fopen(pf, "w"); if (f) { fprintf(f, "%d\n", (int)p); fclose(f); }
    return true;
}

/* 日志一行去掉 "2026-10-12 08:27:02.152 [I] [client/service.go:332] " 这段前缀 */
static void strip(const char *l, char *out, size_t n) {
    const char *p = strstr(l, "] [");
    if (p) { const char *q = strstr(p + 3, "] "); if (q) l = q + 2; }
    snprintf(out, n, "%s", l);
}

/* 只有连接类的行改状态。frp 打的源文件路径只带最后两级: 连接管理在 client/service.go、client/control.go, 它们的 [W]/[E] 才是断线;
   单个请求的告警(httputil/reverseproxy.go 的 "context canceled" = 浏览器自己取消了请求)、插件的 TLS 握手报错(不带级别)与连接无关,
   当成断线的话, 切一下页面就显示"重连中"(10-12 实撞) */
int tr_frpc_line_state(const char *l) {
    if (strstr(l, "start proxy success")) return 2;
    if (strstr(l, "login to server success") || strstr(l, "try to connect")) return 1;
    if ((strstr(l, "[W]") || strstr(l, "[E]")) && strstr(l, "] [client/")) return -1;
    return 0;
}

/* g_mu 持有时调: 读日志新增的完整行(写了一半的那行留到下次) */
static void scan_log(void) {
    char logp[TR_PATH + 32]; frp_path(logp, sizeof logp, "frpc.log");
    const int fd = open(logp, O_RDONLY); if (fd < 0) return;
    struct stat sb;
    if (!fstat(fd, &sb) && sb.st_size < g_log_off) { g_log_off = 0; g_conn = 1; g_conn_line[0] = g_last_line[0] = 0; }   /* 文件被截断(起了新 frpc) */
    char buf[8192];
    for (;;) {
        const ssize_t got = pread(fd, buf, sizeof buf - 1, g_log_off);
        if (got <= 0) break;
        ssize_t use = got; while (use > 0 && buf[use - 1] != '\n') use--;
        if (!use) { if (got < (ssize_t)sizeof buf - 1) break; use = got; }   /* 一行比缓冲还长: 整块当一行 */
        buf[use] = 0;
        for (char *b = buf, *e; *b; b = e) {
            e = b + strcspn(b, "\n"); const char c = *e; *e = 0;
            char *r = b + strlen(b); while (r > b && (r[-1] == '\r' || r[-1] == ' ')) *--r = 0;
            if (*b) {
                strip(b, g_last_line, sizeof g_last_line);
                const int st = tr_frpc_line_state(b);
                if (st) { g_conn = st; snprintf(g_conn_line, sizeof g_conn_line, "%s", g_last_line); }
            }
            if (c) e++;
        }
        g_log_off += use;
    }
    close(fd);
}

int tr_frpc_state(char *line, size_t n, int *exit_code) {
    pthread_mutex_lock(&g_mu);
    scan_log();
    const pid_t p = g_pid; *exit_code = g_exit;
    const int st = p ? g_conn : 0;
    snprintf(line, n, "%s", p ? g_conn_line : g_last_line);   /* 退了: 最后一行就是死因(配置错只打在 stdout, 没有级别标记) */
    pthread_mutex_unlock(&g_mu);
    return st;
}
