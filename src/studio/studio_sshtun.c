/* studio_sshtun.c — frp 控制通道走 SSH(10-12 用户定: "走 SSH, 只开公网端口")。总述见 studio_net.c。
 * 为什么: frpc 直连 frps 的控制口(默认 7000)要求云厂商安全组放行这个口; 7000 被别的程序占着、只能换口时, 用户还得再去控制台开一个
 *   (10-12 用户的服务器实撞: 另一个项目的 frps 占着 7000, 我们换到 7001, 卡在安全组)。改成本机拉一条 ssh -L 端口转发,
 *   frpc 连本机这一头, 服务器那头的 frps 控制口只听 127.0.0.1 —— 安全组只要 22(本来就开)+ 公网端口, 控制口也不再暴露在公网。
 * 守护: ssh 退了(断网、服务器重启、sshd 丢连接)按 2 → 4 → 8 … 最长 30 秒的间隔重拉; 一次连接活过 60 秒就把间隔重置。
 *   本机这一头的端口第一次起时选一个空闲口, 之后重拉都用同一个: frpc 配置里写死了它, 换口就得连 frpc 一起重起。
 *   frpc 自己也一直重连(loginFailExit = false), 两边各管各的, 不用互相通知。
 * pid 记在 frp/sshfwd.pid: 主进程被 kill -9 后 ssh 还活着, 下次起来先按它收掉, 不然两条转发抢同一个本机口。
 * 出错会怎样: 服务器关了端口转发(sshd_config 里 AllowTcpForwarding no)时 ssh 登得上, 但 frpc 一连就报
 *   "administratively prohibited", 日志 frp/sshfwd.log 的最后一行会写这句, 页面据此提示。 */
#include "studio_internal.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TR_FWD_BACKOFF_MAX 30   /* 重拉间隔上限(秒): 服务器长时间连不上时别每两秒敲一次它的 sshd */
#define TR_FWD_STABLE_S 60      /* 活过这么久算"连上过", 下次断了从 2 秒重新数 */
#define TR_FWD_READY_S 25       /* 起转发后等本机口能连上: ssh 握手 + 密码认证, 跨境线路也就几秒; 多给是因为 sshd 偶发丢连接要重拉 */

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pid_t g_pid;
static int g_lport;          /* 本机这一头(0 = 没起) */
static unsigned g_gen;       /* 每 start 一次 +1, 老守护线程看到对不上就退 */
static char g_args[5][300];  /* 当前转发的参数: host / port / user / 目标地址 / 目标端口(守护线程重拉用) */

static void fwd_path(char *out, size_t n, const char *leaf) { snprintf(out, n, "%s/frp/%s", tr_net_dir(), leaf); }

static int free_port(void) {   /* 让内核挑一个空闲口再关掉: 之后 ssh 立刻绑上去, 中间被别人抢走的窗口只有毫秒级 */
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t l = sizeof a; int p = 0;
    if (s >= 0 && !bind(s, (struct sockaddr *)&a, sizeof a) && !getsockname(s, (struct sockaddr *)&a, &l)) p = ntohs(a.sin_port);
    if (s >= 0) close(s);
    return p;
}

static pid_t spawn(int lport) {
    char kh[TR_PATH + 64], khopt[TR_PATH + 96], fwd[400], logp[TR_PATH + 32];
    snprintf(kh, sizeof kh, "%s/known_hosts", tr_net_dir()); snprintf(khopt, sizeof khopt, "UserKnownHostsFile=%s", kh);
    const bool v6 = strchr(g_args[3], ':') != NULL;
    snprintf(fwd, sizeof fwd, v6 ? "127.0.0.1:%d:[%s]:%s" : "127.0.0.1:%d:%s:%s", lport, g_args[3], g_args[4]);
    fwd_path(logp, sizeof logp, "sshfwd.log");
    const char *argv[] = { "ssh", "-N", "-p", g_args[1], "-l", g_args[2], "-o", "StrictHostKeyChecking=accept-new", "-o", khopt,
        "-o", "ConnectTimeout=10", "-o", "PubkeyAuthentication=no", "-o", "PreferredAuthentications=password,keyboard-interactive",
        "-o", "NumberOfPasswordPrompts=1", "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=3", "-o", "ExitOnForwardFailure=yes",
        "-o", "LogLevel=ERROR", "-L", fwd, "--", g_args[0], NULL };
    return tr_spawn_bg(argv, logp, TR_EXEC_ASKPASS);
}

static void log_line(const char *msg) {
    char p[TR_PATH + 32]; fwd_path(p, sizeof p, "sshfwd.log");
    FILE *f = fopen(p, "a"); if (f) { tr_logf(f, "%s", msg); fclose(f); }
}

static void *guard(void *a) {
    const unsigned gen = (unsigned)(uintptr_t)a;
    int backoff = 2;
    for (;;) {
        pthread_mutex_lock(&g_mu); const pid_t p = g_gen == gen ? g_pid : 0; const int lp = g_lport; pthread_mutex_unlock(&g_mu);
        if (!p) return NULL;
        const time_t t0 = time(NULL);
        int st = 0; while (waitpid(p, &st, 0) < 0 && errno == EINTR) {}
        pthread_mutex_lock(&g_mu); const bool live = g_gen == gen; if (live) g_pid = 0; pthread_mutex_unlock(&g_mu);
        if (!live) return NULL;   /* stop() 收的: 不重拉 */
        if (time(NULL) - t0 >= TR_FWD_STABLE_S) backoff = 2;
        char m[96]; snprintf(m, sizeof m, "ssh exited (status %d), reconnect in %d s", WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st), backoff);
        log_line(m);
        sleep((unsigned)backoff);
        backoff = backoff * 2 > TR_FWD_BACKOFF_MAX ? TR_FWD_BACKOFF_MAX : backoff * 2;
        pthread_mutex_lock(&g_mu);
        if (g_gen != gen) { pthread_mutex_unlock(&g_mu); return NULL; }
        g_pid = spawn(lp);
        pthread_mutex_unlock(&g_mu);
    }
}

static bool is_ssh(pid_t p) {
    char b[64]; snprintf(b, sizeof b, "/proc/%d/cmdline", (int)p);
    char *s = tr_slurp(b, NULL);   /* Linux; Mac 没有 /proc —— 按 pid 文件收旧进程只在 Linux 上做, Mac 上主进程退出时 shutdown 会收 */
    const bool ok = s && !strcmp(s, "ssh") && strstr(s + 4, "-N") != NULL;
    free(s);
    return ok;
}

void tr_sshfwd_stop(void) {
    pthread_mutex_lock(&g_mu); g_gen++; const pid_t p = g_pid; g_pid = 0; g_lport = 0; pthread_mutex_unlock(&g_mu);
    if (p > 0) { kill(p, SIGTERM); for (int i = 0; i < 20 && !kill(p, 0); i++) usleep(100000); if (!kill(p, 0)) kill(p, SIGKILL); waitpid(p, NULL, WNOHANG); }
    char pf[TR_PATH + 32]; fwd_path(pf, sizeof pf, "sshfwd.pid");
    char *s = tr_slurp(pf, NULL);
    if (s) { const pid_t old = (pid_t)atoi(s); free(s); if (old > 0 && old != p && is_ssh(old)) kill(old, SIGTERM); }
    unlink(pf);
}

int tr_sshfwd_start(const tr_cloud *c, const char *target, int tport, char *err, size_t en) {
    tr_sshfwd_stop();
    const int lp = free_port();
    if (!lp) { snprintf(err, en, "sshfwd: no free local port"); return 0; }
    char logp[TR_PATH + 32]; fwd_path(logp, sizeof logp, "sshfwd.log");
    const int lf = open(logp, O_WRONLY | O_CREAT | O_TRUNC, 0600); if (lf >= 0) close(lf);
    pthread_mutex_lock(&g_mu);
    const unsigned gen = ++g_gen;
    snprintf(g_args[0], sizeof g_args[0], "%s", c->host); snprintf(g_args[1], sizeof g_args[1], "%d", c->port);
    snprintf(g_args[2], sizeof g_args[2], "%s", c->user); snprintf(g_args[3], sizeof g_args[3], "%s", target && target[0] ? target : "127.0.0.1");
    snprintf(g_args[4], sizeof g_args[4], "%d", tport);
    g_lport = lp; g_pid = spawn(lp);
    const pid_t p = g_pid;
    pthread_mutex_unlock(&g_mu);
    if (p < 0) { snprintf(err, en, "sshfwd: fork: %s", strerror(errno)); return 0; }
    char pf[TR_PATH + 32]; fwd_path(pf, sizeof pf, "sshfwd.pid");
    FILE *f = fopen(pf, "w"); if (f) { fprintf(f, "%d\n", (int)p); fclose(f); }
    pthread_t th; pthread_create(&th, NULL, guard, (void *)(uintptr_t)gen); pthread_detach(th);
    for (int i = 0; i < TR_FWD_READY_S * 2; i++) {   /* 本机口能连上 = ssh 认证过了、转发绑好了(ExitOnForwardFailure: 绑不上 ssh 直接退) */
        if (tr_tcp_probe("127.0.0.1", lp, 1)) return lp;
        usleep(500000);
    }
    char line[300]; tr_sshfwd_state(line, sizeof line);
    snprintf(err, en, "sshfwd: %s", line[0] ? line : "timeout");
    tr_sshfwd_stop();
    return 0;
}

int tr_sshfwd_port(void) { pthread_mutex_lock(&g_mu); const int p = g_lport; pthread_mutex_unlock(&g_mu); return p; }

bool tr_sshfwd_state(char *line, size_t n) {
    line[0] = 0;
    char p[TR_PATH + 32]; fwd_path(p, sizeof p, "sshfwd.log");
    size_t len = 0; char *s = tr_slurp(p, &len);
    if (s) {   /* 最后一个非空行 */
        char *e = s + len; while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        char *b = e; while (b > s && b[-1] != '\n') b--;
        snprintf(line, n, "%s", b); free(s);
    }
    pthread_mutex_lock(&g_mu); const bool up = g_pid > 0; pthread_mutex_unlock(&g_mu);
    return up;
}
