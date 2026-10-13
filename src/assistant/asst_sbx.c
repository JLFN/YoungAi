/* asst_sbx.c — 沙盒子进程的起、收、杀(平台无关部分)。总述见 asst_internal.h。
 * 协议: 子进程往管道先写 1 个字节 —— 'O' = 已上锁, 后面跟结果文本; 'E' = 上锁失败, 后面跟原因。父进程没读到 'O' 就当没跑过。
 * 为什么每次调用都 fork 新进程而不是起一个常驻的沙盒工人: 规则是按这一次点名的路径定的(审批"仅这次"给的路径下一次就不能再碰),
 *   Landlock / Seatbelt 都只能收紧不能放宽, 常驻进程做不到一次一套规则。fork 一次 ~1 ms, 跟模型生成一个 token 比可以忽略。
 * 出错会怎样: 超时 → 整个进程组 SIGKILL, 结果是"timeout after N s"加已经读到的部分; 子进程被信号杀 → ASST_SBX_CRASHED。 */
#include "asst_internal.h"
#include <dirent.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#endif

bool asst_sbx_add(asst_sbx_spec *sp, bool rw, const char *path) {
    char real[PATH_MAX];
    if (!path || !realpath(path, real) || strlen(real) >= ASST_PATH) return false;   /* 截断的路径 = 另一个路径, 宁可不给 */
    if (rw) { if (sp->nrw >= ASST_SBX_MAX_ROOTS) return false; memcpy(sp->rw[sp->nrw++], real, strlen(real) + 1); }
    else    { if (sp->nro >= ASST_SBX_MAX_ROOTS) return false; memcpy(sp->ro[sp->nro++], real, strlen(real) + 1); }
    return true;
}

/* 本用户现在有多少个任务: RLIMIT_NPROC 按真实 uid 的总数算, 不是"这个沙盒开了几个", 所以得在现有数上加余量。
 * ★Linux 数的是线程不是进程★(10-11 spark 实撞: 按进程数 +64 设, 桌面会话与 ds4-train 的线程一算就已超, bash 一个都 fork 不出来); macOS 数的是进程。 */
static long user_nproc(void) {
    long n = 0;
#ifdef __APPLE__
    int cnt = proc_listpids(PROC_UID_ONLY, getuid(), NULL, 0);
    n = cnt > 0 ? cnt / (int)sizeof(pid_t) : 0;
#else
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *e; char p[300], line[256];
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        snprintf(p, sizeof p, "/proc/%s/status", e->d_name);
        FILE *f = fopen(p, "r"); if (!f) continue;
        bool mine = false;   /* status 里 Uid: 在 Threads: 之前 */
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "Uid:", 4)) { mine = (uid_t)strtoul(line + 4, NULL, 10) == getuid(); if (!mine) break; }
            else if (mine && !strncmp(line, "Threads:", 8)) { n += strtol(line + 8, NULL, 10); break; }
        }
        fclose(f);
    }
    closedir(d);
#endif
    return n;
}

/* 父进程开着的 fd(模型子进程的 socket、日志、浏览器连接)一个都不许带进沙盒: 带进去就是一条绕过文件规则的现成通道 */
static void close_inherited(int keep, int keep2) {
#ifdef __APPLE__
    const char *fddir = "/dev/fd";
#else
    const char *fddir = "/proc/self/fd";
#endif
    int fds[4096], n = 0;
    DIR *d = opendir(fddir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && n < 4096) { int fd = atoi(e->d_name); if (e->d_name[0] >= '0' && e->d_name[0] <= '9' && fd > 2 && fd != keep && fd != keep2) fds[n++] = fd; }
        closedir(d);
        for (int i = 0; i < n; i++) close(fds[i]);   /* 先收集再关: 边读目录边关会把 opendir 自己的 fd 关掉 */
    } else {
        for (int fd = 3; fd < 65536; fd++) if (fd != keep && fd != keep2) close(fd);
    }
}

static void set_limits(const asst_sbx_spec *sp, long nproc) {
    struct rlimit r;
    r.rlim_cur = r.rlim_max = 0; setrlimit(RLIMIT_CORE, &r);
    r.rlim_cur = r.rlim_max = ASST_SBX_FSIZE_MAX; setrlimit(RLIMIT_FSIZE, &r);
    r.rlim_cur = r.rlim_max = (rlim_t)(sp->timeout_s > 0 ? sp->timeout_s + 2 : 62); setrlimit(RLIMIT_CPU, &r);
    r.rlim_cur = r.rlim_max = 256; setrlimit(RLIMIT_NOFILE, &r);
    if (nproc > 0) { r.rlim_cur = r.rlim_max = (rlim_t)(nproc + ASST_SBX_NPROC_EXTRA); setrlimit(RLIMIT_NPROC, &r); }
#ifndef __APPLE__
    r.rlim_cur = r.rlim_max = ASST_SBX_AS_MAX; setrlimit(RLIMIT_AS, &r);   /* macOS 不执行 RLIMIT_AS, 设了也白设 */
#endif
}

static void write_all_fd(int fd, const char *p, size_t n) {
    while (n) { ssize_t w = write(fd, p, n); if (w < 0) { if (errno == EINTR) continue; return; } p += w; n -= (size_t)w; }
}

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec * 1e-9; }

/* 子进程崩了把栈写回结果管道再按默认动作死: 沙盒里 RLIMIT_CORE=0 拿不到 core, 父进程只看到"killed by signal 11"(10-13 spark 实撞: 真请求的 make_video
 * 子进程段错误, 沙盒外同样输入全正常, 没有栈无从下手)。只用 async-signal-safe 的东西: backtrace_symbols_fd 直接写 fd, 地址用 addr2line -e 二进制 解。 */
static int crash_fd = -1;
static void on_crash(int sig) {
    char head[64]; const int n = snprintf(head, sizeof head, "\n[crash: signal %d, stack:]\n", sig);
    if (crash_fd >= 0) { write_all_fd(crash_fd, head, (size_t)n); void *bt[48]; const int k = backtrace(bt, 48); backtrace_symbols_fd(bt, k, crash_fd); }
    signal(sig, SIG_DFL); raise(sig);
}

asst_sbx_rc asst_sbx_run(const asst_sbx_spec *sp, asst_sbx_fn fn, void *ud, char **out) {
    *out = NULL;
    int pf[2];
    if (pipe(pf) < 0) { *out = strdup("sandbox: pipe failed"); return ASST_SBX_SPAWN_FAILED; }
    const long nproc = user_nproc();
    fflush(NULL);   /* 不然父进程 stdio 缓冲里没刷的字节会在子进程退出时再刷一遍 */
    const pid_t pid = fork();
    if (pid < 0) { close(pf[0]); close(pf[1]); *out = strdup("sandbox: fork failed"); return ASST_SBX_SPAWN_FAILED; }
    if (pid == 0) {
        close(pf[0]);
        setsid();   /* 自成进程组: 超时与收尾时 killpg 一把带走 bash 留下的后台进程 */
        signal(SIGPIPE, SIG_DFL);
        const int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); if (nul > 2) close(nul); }   /* stderr 留着: 上锁失败的诊断进 ds4-train 日志 */
        close_inherited(pf[1], sp->keep_fd > 2 ? sp->keep_fd : -1);
        fcntl(pf[1], F_SETFD, FD_CLOEXEC);   /* bash exec 出去的进程拿不到结果管道, 不然后台进程挂着它父进程永远等不到 EOF */
        set_limits(sp, nproc);
        char err[512] = "";
        if (!asst_sbx_lock(sp, err, sizeof err)) {
            write_all_fd(pf[1], "E", 1); write_all_fd(pf[1], err, strlen(err));
            _exit(97);
        }
        write_all_fd(pf[1], "O", 1);
        crash_fd = pf[1]; signal(SIGSEGV, on_crash); signal(SIGBUS, on_crash); signal(SIGFPE, on_crash); signal(SIGABRT, on_crash); signal(SIGILL, on_crash);
        char *r = fn(ud);
        if (r) { size_t n = strlen(r); if (n > ASST_SBX_OUT_MAX) n = ASST_SBX_OUT_MAX; write_all_fd(pf[1], r, n); }
        _exit(0);
    }
    close(pf[1]);
    ds4_buf b = {0};
    const double deadline = now_s() + (sp->timeout_s > 0 ? sp->timeout_s : 60);
    bool timed_out = false;
    char tmp[16384];
    for (;;) {
        const double left = deadline - now_s();
        if (left <= 0) { timed_out = true; break; }
        struct pollfd p = { pf[0], POLLIN, 0 };
        const int pr = poll(&p, 1, (int)(left * 1000) + 1);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;
        const ssize_t n = read(pf[0], tmp, sizeof tmp);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) break;
        if (b.len < ASST_SBX_OUT_MAX + 1) ds4_buf_append(&b, tmp, (size_t)n);
    }
    close(pf[0]);
    kill(-pid, SIGKILL);   /* 正常结束也杀一遍组: bash 里 `sleep 999 &` 这类不许活过这次调用 */
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    ds4_buf_putc(&b, 0); b.len--;
    if (!b.len || (b.ptr[0] != 'O' && b.ptr[0] != 'E')) {
        ds4_buf_free(&b);
        if (timed_out) { *out = strdup("sandbox: timeout before lock"); return ASST_SBX_TIMEOUT; }
        *out = strdup(WIFSIGNALED(st) ? "sandbox: child killed by signal before lock" : "sandbox: child exited before lock");
        return ASST_SBX_CRASHED;
    }
    if (b.ptr[0] == 'E') {
        ds4_buf r = {0}; ds4_buf_printf(&r, "sandbox lock failed (%s): %s", asst_sbx_kind(), b.ptr + 1);
        ds4_buf_free(&b); *out = ds4_buf_take(&r);
        return ASST_SBX_LOCK_FAILED;
    }
    ds4_buf r = {0};
    ds4_buf_append(&r, b.ptr + 1, b.len - 1);
    ds4_buf_free(&b);
    asst_sbx_rc rc = ASST_SBX_OK;
    if (timed_out) { ds4_buf_printf(&r, "%s[timeout after %d s; process group killed]\n", r.len && r.ptr[r.len - 1] != '\n' ? "\n" : "", sp->timeout_s); rc = ASST_SBX_TIMEOUT; }
    else if (WIFSIGNALED(st)) { ds4_buf_printf(&r, "%s[tool process killed by signal %d]\n", r.len ? "\n" : "", WTERMSIG(st)); rc = ASST_SBX_CRASHED; }
    *out = ds4_buf_take(&r);
    return rc;
}
