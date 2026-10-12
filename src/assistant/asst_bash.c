/* asst_bash.c — bash 工具本体: 在已上锁的沙盒子进程里再 fork 出 bash(锁会继承, bash 和它开的一切都在同一套规则里)。
 * 环境变量全换成干净的五个(PATH/HOME/TMPDIR/LANG/TERM): ds4-train 的环境里可能有 HF_TOKEN、代理口令, 一个 `env` 就全吐给模型了。
 *   HOME 指向工作区: 工具写 ~/.cache 之类落在工作区里(沙盒外的真家目录本来也写不了)。
 * 超时由这里自己管(比外层 asst_sbx_run 的墙钟早 3 秒): 到点杀 bash 的进程组, 已经输出的部分照样交回, 模型能看到卡在哪。
 * 输出只留头 64 KB + 尾 8 KB: 编译刷屏时错误常在最后。 */
#include "asst_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BASH_HEAD (64u << 10)
#define BASH_TAIL (8u << 10)

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec * 1e-9; }

char *asst_bash_exec(void *ap) {
    const asst_bash_args *a = ap;
    char tmpdir[ASST_PATH + 8]; snprintf(tmpdir, sizeof tmpdir, "%s/.tmp", a->cwd);
    mkdir(tmpdir, 0700);
    char e_home[ASST_PATH + 8], e_tmp[ASST_PATH + 16];
    snprintf(e_home, sizeof e_home, "HOME=%s", a->cwd);
    snprintf(e_tmp, sizeof e_tmp, "TMPDIR=%s", tmpdir);
#ifdef __APPLE__
    char *const envp[] = { "PATH=" ASST_SBX_PATH, e_home, e_tmp, "LANG=en_US.UTF-8", "TERM=dumb", NULL };   /* 与 asst_toolbox.c 探测用的同一条 PATH: 探到有的, 这里一定找得到 */
#else
    char *const envp[] = { "PATH=" ASST_SBX_PATH, e_home, e_tmp, "LANG=C.UTF-8", "TERM=dumb", NULL };
#endif
    int pf[2];
    if (pipe(pf) < 0) return strdup("error: pipe failed\n");
    const pid_t pid = fork();
    if (pid < 0) return strdup("error: fork failed (process limit reached?)\n");
    if (pid == 0) {
        setpgid(0, 0);
        if (chdir(a->cwd)) _exit(126);
        const int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) dup2(nul, 0);
        dup2(pf[1], 1); dup2(pf[1], 2);
        close(pf[0]); close(pf[1]);
        char *const argv[] = { "bash", "-c", (char *)a->command, NULL };
        execve("/bin/bash", argv, envp);
        _exit(127);
    }
    close(pf[1]);
    const int tmo = a->timeout_s > 0 ? a->timeout_s : 60;
    const double deadline = now_s() + tmo;
    ds4_buf head = {0}; char tail[BASH_TAIL]; size_t tail_n = 0, total = 0;
    bool timed_out = false, exited = false; int st = 0;
    char buf[16384];
    for (;;) {
        const double left = deadline - now_s();
        if (left <= 0) { timed_out = true; break; }
        struct pollfd p = { pf[0], POLLIN, 0 };
        const int pr = poll(&p, 1, left < 0.2 ? (int)(left * 1000) + 1 : 200);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (!exited && waitpid(pid, &st, WNOHANG) == pid) exited = true;
        if (pr == 0) { if (exited) break; continue; }   /* bash 已退、200 ms 没新输出: 后台进程占着管道不出声, 不等它 */
        const ssize_t n = read(pf[0], buf, sizeof buf);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) break;   /* EOF: bash 与它的后台进程都关了输出 */
        total += (size_t)n;
        for (ssize_t i = 0; i < n; i++) {
            if (head.len < BASH_HEAD) { ds4_buf_putc(&head, buf[i]); continue; }
            if (tail_n < BASH_TAIL) tail[tail_n++] = buf[i]; else { memmove(tail, tail + 1, BASH_TAIL - 1); tail[BASH_TAIL - 1] = buf[i]; }
        }
    }
    close(pf[0]);
    kill(-pid, SIGKILL);
    if (!exited) { while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {} }
    ds4_buf r = {0};
    if (timed_out) ds4_buf_printf(&r, "[timed out after %d s; killed]\n", tmo);
    else if (WIFEXITED(st)) ds4_buf_printf(&r, "exit code %d\n", WEXITSTATUS(st));
    else if (WIFSIGNALED(st)) ds4_buf_printf(&r, "killed by signal %d\n", WTERMSIG(st));
    ds4_buf_append(&r, head.ptr ? head.ptr : "", head.len);
    if (total > head.len + tail_n) ds4_buf_printf(&r, "\n…[%zu bytes cut]…\n", total - head.len - tail_n);
    ds4_buf_append(&r, tail, tail_n);
    ds4_buf_free(&head);
    return ds4_buf_take(&r);
}
