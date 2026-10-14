/* studio_exec.c — 设置弹窗(studio_net.c)要跑的外部命令: ssh、curl、tar、unzip、openssl, 以及常驻的 frpc。
 * 全部 fork + execvp 直接跑, 不经本机 shell: 参数里有什么字符都不会被当成命令解释。输出 stdout+stderr 合并收回来, 出错时原样给页面看。
 * ssh 的密码: 不进命令行(ps 看得到)也不进环境变量, 而是让 ssh 去问"askpass 程序" —— 就是 ds4-train 自己:
 *   ~/.ds4/studio/ds4-askpass 是指向 ds4-train 的符号链接, main 见 argv[0] 叫这个名字, 只把设置文件里的密码打到 stdout 就退(tr_askpass_main)。
 *   SSH_ASKPASS / SSH_ASKPASS_REQUIRE=force 是 OpenSSH(≥ 8.4)自己的接口, 只设给 ssh 这一个子进程, 不是我们读的配置。
 * frpc 去掉 http_proxy 一类变量再起: frpc 会照这些变量走代理, Mac 上开着 Clash 时 frpc 连云服务器会被转去代理节点, 实测直接
 *   "connect to server error: EOF"(10-12)。curl 下 frp 反而要留着代理(GitHub 在国内要靠它)。
 * 出错会怎样: 命令不存在 → 退出码 127, 输出里有 "exec <名字> failed"; 超时 → 整个进程组被 SIGKILL, 返回 TR_EXEC_TIMEOUT。 */
#include "studio_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TR_EXEC_OUT_MAX (1u << 20)   /* 收回的输出封顶 1 MB: 报错只看尾巴, curl/tar 正常时几乎没输出 */

static char g_askpass[TR_PATH + 32];
void tr_exec_set_askpass(const char *path) { snprintf(g_askpass, sizeof g_askpass, "%s", path); }

/* 子进程这一侧: 新会话(没有控制终端, ssh 才不会去开 /dev/tty 问密码)、fd 重定向、只关 fd ≥ 3 不动 0/1/2 */
static void child_setup(int in_fd, int out_fd, unsigned flags) {
    setsid();
    const int nul = open("/dev/null", O_RDWR);
    dup2(in_fd >= 0 ? in_fd : nul, 0); dup2(out_fd >= 0 ? out_fd : nul, 1); dup2(out_fd >= 0 ? out_fd : nul, 2);
    long mx = sysconf(_SC_OPEN_MAX); if (mx < 0 || mx > 4096) mx = 4096;
    for (int fd = 3; fd < mx; fd++) close(fd);
    if (flags & TR_EXEC_ASKPASS) { setenv("SSH_ASKPASS", g_askpass, 1); setenv("SSH_ASKPASS_REQUIRE", "force", 1); setenv("DISPLAY", ":0", 0); }
    if (flags & TR_EXEC_NOPROXY) {
        static const char *const pv[] = { "http_proxy", "https_proxy", "all_proxy", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "no_proxy", "NO_PROXY" };
        for (size_t i = 0; i < sizeof pv / sizeof pv[0]; i++) unsetenv(pv[i]);
    }
}

int tr_exec(const char *const argv[], const char *in, size_t in_len, ds4_buf *out, int timeout_s, unsigned flags) {
    int po[2], pi[2] = { -1, -1 };
    if (pipe(po)) return -1;
    if (in && pipe(pi)) { close(po[0]); close(po[1]); return -1; }
    const pid_t pid = fork();
    if (pid < 0) { close(po[0]); close(po[1]); if (in) { close(pi[0]); close(pi[1]); } return -1; }
    if (pid == 0) {
        child_setup(in ? pi[0] : -1, po[1], flags);
        execvp(argv[0], (char *const *)argv);
        dprintf(2, "exec %s failed: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(po[1]);
    if (in) { close(pi[0]); fcntl(pi[1], F_SETFL, O_NONBLOCK); }
    size_t sent = 0; bool in_open = in != NULL, timed_out = false;
    if (in_open && !in_len) { close(pi[1]); in_open = false; }
    const time_t deadline = time(NULL) + timeout_s;
    char tmp[16384];
    for (;;) {
        struct pollfd pf[2] = { { po[0], POLLIN, 0 }, { in_open ? pi[1] : -1, POLLOUT, 0 } };
        const int pr = poll(pf, in_open ? 2 : 1, 200);
        if (pr < 0 && errno != EINTR) break;
        if (in_open && (pf[1].revents & (POLLOUT | POLLERR | POLLHUP))) {   /* 喂 stdin(大文件经 ssh 的 cat 上传也走这里): 边写边读, 不会两头互等 */
            const ssize_t w = write(pi[1], in + sent, in_len - sent);
            if (w > 0) sent += (size_t)w;
            if ((w < 0 && errno != EAGAIN && errno != EINTR) || sent == in_len) { close(pi[1]); in_open = false; }
        }
        if (pf[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            const ssize_t n = read(po[0], tmp, sizeof tmp);
            if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) break;
            if (n > 0 && out && out->len < TR_EXEC_OUT_MAX) ds4_buf_append(out, tmp, (size_t)n);
        }
        if (time(NULL) >= deadline) { timed_out = true; kill(-pid, SIGKILL); kill(pid, SIGKILL); break; }
    }
    if (in_open) close(pi[1]);
    close(po[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (timed_out) return TR_EXEC_TIMEOUT;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

pid_t tr_spawn_bg(const char *const argv[], const char *log_path, unsigned flags) {
    const int lf = log_path ? open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0600) : -1;
    const pid_t pid = fork();
    if (pid == 0) {
        child_setup(-1, lf, flags);
        execvp(argv[0], (char *const *)argv);
        dprintf(2, "exec %s failed: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    if (lf >= 0) close(lf);
    return pid;
}

/* 连得上 host:port 吗(非阻塞 connect + poll 等 timeout_s 秒)。只在部署线程里调: 解析域名可能卡几秒, 不能放进单线程的主循环 */
bool tr_tcp_probe(const char *host, int port, int timeout_s) {
    char ps[16]; snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hint, *res = NULL; memset(&hint, 0, sizeof hint);
    hint.ai_family = AF_UNSPEC; hint.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, ps, &hint, &res) || !res) return false;
    bool ok = false;
    for (struct addrinfo *a = res; a && !ok; a = a->ai_next) {
        const int s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s < 0) continue;
        fcntl(s, F_SETFL, O_NONBLOCK);
        if (!connect(s, a->ai_addr, a->ai_addrlen)) ok = true;
        else if (errno == EINPROGRESS) {
            struct pollfd pf = { s, POLLOUT, 0 };
            int err = 0; socklen_t el = sizeof err;
            if (poll(&pf, 1, timeout_s * 1000) == 1 && !getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) && !err) ok = true;
        }
        close(s);
    }
    freeaddrinfo(res);
    return ok;
}

/* host 解析出的 IPv4 地址, 逗号分隔写进 out(没有 = 空串)。证书域名要解析到云服务器, 对不上就提醒 */
void tr_resolve4(const char *host, char *out, size_t n) {
    out[0] = 0;
    struct addrinfo hint, *res = NULL; memset(&hint, 0, sizeof hint);
    hint.ai_family = AF_INET; hint.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hint, &res) || !res) return;
    size_t o = 0;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        char ip[64];
        if (getnameinfo(a->ai_addr, a->ai_addrlen, ip, sizeof ip, NULL, 0, NI_NUMERICHOST)) continue;
        if (strstr(out, ip)) continue;   /* 同一地址按 socktype 会出好几条 */
        const int w = snprintf(out + o, n - o, "%s%s", o ? "," : "", ip);
        if (w < 0 || (size_t)w >= n - o) break;
        o += (size_t)w;
    }
    freeaddrinfo(res);
}
