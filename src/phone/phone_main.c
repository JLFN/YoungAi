/* phone_main.c — ds4-phone 入口: start | stop | status(总述见 phone_internal.h)。
 * start 的顺序(每一步为什么在这个位置):
 *   ① 建 cgroup(父进程自己不进去: 失败清理走 cgroup.kill, 自己在里面就把自己杀了 —— 10-13 实撞 exit 137);
 *   ② clone(CLONE_NEWPID): 子进程成为新 pid 命名空间的 1 号(Android 的 init 必须是 pid 1), 父进程留在宿主命名空间;
 *   ③ 子进程 unshare 挂载/网络/uts/ipc 后在管道上报"命名空间好了";
 *   ④ 父进程把子进程 pid 写进 cgroup, 把 veth 的一头塞进它的网络命名空间(ip link … netns <pid>)并在宿主侧装 NAT; 装完在管道上回"好了";
 *   ⑤ 子进程这时才 unshare(CLONE_NEWCGROUP)(命名空间的根 = 此刻所在的 cgroup, 手机只看得见自己这棵), 配 eth0, 挂载表, pivot_root, exec /init;
 *      父进程等 adb 口能连上(最多 90 s)就退出; 手机留在 cgroup 里。
 * stop: cgroup.kill 杀整棵树(不靠 pid, 不怕 init 已经 fork 出几十个进程) → 拆网 → 删 cgroup。手机的 /data 不动。
 * status: 一行 JSON 给 ds4-train 解析。 */
#include "phone_internal.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/klog.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define START_WAIT_S 90   /* 冷启动(页缓存冷, SSD 读 1.5 GB 的 rootfs)到 adbd 监听; 热启动 10~20 s */

char ph_root[PH_PATH], ph_dir[PH_PATH + 32];

void ph_log(const char *fmt, ...) {
    char p[PH_PATH + 96]; snprintf(p, sizeof p, "%s/phone.log", ph_dir);
    FILE *f = fopen(p, "a");
    char ts[32]; time_t t = time(NULL); strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
    va_list ap; va_start(ap, fmt);
    if (f) { fprintf(f, "[%s ds4-phone] ", ts); vfprintf(f, fmt, ap); fputc('\n', f); fclose(f); }
    va_end(ap);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap);
}

int ph_run(char *const argv[], bool quiet) {
    const pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (quiet) { const int nul = open("/dev/null", O_WRONLY); if (nul >= 0) { dup2(nul, 2); close(nul); } }   /* 拆残留时 ip/iptables 的"没这条规则"不往终端喷 */
        execvp(argv[0], argv); _exit(127);
    }
    int st = 0; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    const int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    if (code && !quiet) {
        char line[512] = ""; size_t L = 0;
        for (int i = 0; argv[i] && L < sizeof line - 2; i++) L += (size_t)snprintf(line + L, sizeof line - L, "%s ", argv[i]);
        ph_log("命令失败(%d): %s", code, line);
    }
    return code;
}

static void find_root(void) {
    char exe[PH_PATH]; const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) { fprintf(stderr, "ds4-phone: readlink /proc/self/exe 失败\n"); exit(1); }
    exe[n] = 0;
    char *sl = strrchr(exe, '/'); if (sl) *sl = 0;
    snprintf(ph_root, sizeof ph_root, "%s", exe);
    snprintf(ph_dir, sizeof ph_dir, "%s/gguf/phone", ph_root);
}

static bool adb_listening(void) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) return false;
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(PH_ADB_PORT) }; inet_pton(AF_INET, PH_IP_PHONE, &sa.sin_addr);
    struct timeval tv = { 1, 0 }; setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    const bool ok = connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0;
    close(fd); return ok;
}

static int cmd_status(void) {
    pid_t first = 0; const int n = ph_cg_exists() ? ph_cg_pids(&first) : 0;
    long started = 0; char p[PH_PATH + 96]; snprintf(p, sizeof p, "%s/started", ph_dir);
    FILE *f = fopen(p, "r"); if (f) { if (fscanf(f, "%ld", &started) != 1) started = 0; fclose(f); }
    const long up = n && started ? (long)time(NULL) - started : 0;
    printf("{\"running\":%s,\"pids\":%d,\"init_pid\":%ld,\"mem_mb\":%ld,\"peak_mb\":%ld,\"cap_mb\":%ld,\"uptime_s\":%ld,\"adb\":%s,\"ip\":\"%s\",\"port\":%d,\"width\":%d,\"height\":%d}\n",
           n ? "true" : "false", n, (long)first, n ? ph_cg_mem_mb(false) : 0, n ? ph_cg_mem_mb(true) : 0, n ? ph_cg_cap_mb() : 0, up, n && adb_listening() ? "true" : "false",
           PH_IP_PHONE, PH_ADB_PORT, PH_WIDTH, PH_HEIGHT);
    return 0;
}

/* 手机里 Android init 的日志只写 /dev/kmsg = 宿主内核环缓冲; spark 的 kernel.dmesg_restrict=1 普通用户读不到, 本程序有 root 就顺手给一个只读口:
 * 只打带 "init:" / "binder" / "ds4-phone" 字样的行, 别的内核日志不外泄 */
static int cmd_dmesg(void) {
    const int n = klogctl(10, NULL, 0);   /* SYSLOG_ACTION_SIZE_BUFFER */
    if (n <= 0) { fprintf(stderr, "ds4-phone: klogctl: %s(要 root)\n", strerror(errno)); return 1; }
    char *buf = malloc((size_t)n + 1); if (!buf) return 1;
    const int got = klogctl(3, buf, n);   /* SYSLOG_ACTION_READ_ALL, 不清缓冲 */
    if (got < 0) { fprintf(stderr, "ds4-phone: klogctl read: %s\n", strerror(errno)); free(buf); return 1; }
    buf[got] = 0;
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n"))
        if (strstr(line, "init:") || strstr(line, "binder") || strstr(line, "ds4-phone") || strstr(line, "Android") || strstr(line, "ueventd") || strstr(line, "audit") || strstr(line, "selinux")
            || strstr(line, "oom") || strstr(line, "Out of memory") || strstr(line, "Killed process")) puts(line);
    free(buf);
    return 0;
}

static int cmd_stop(void) {
    if (!ph_cg_exists()) { ph_net_down(); printf("not running\n"); return 0; }
    const bool ok = ph_cg_kill();
    ph_net_down();
    ph_log("stop: %s", ok ? "手机已停, cgroup 已删" : "cgroup 杀不干净(看 phone.log)");
    printf("%s\n", ok ? "stopped" : "stop failed");
    return ok ? 0 : 1;
}

static int cmd_start(void) {
    char rootfs[PH_PATH + 96], err[256] = "";
    snprintf(rootfs, sizeof rootfs, "%s/rootfs/system/bin/init", ph_dir);   /* rootfs/init 是指向 /system/bin/init 的绝对软链, 在宿主上解析不到 */
    if (access(rootfs, X_OK)) { fprintf(stderr, "ds4-phone: %s 不在 —— 先 scripts/phone_fetch_rootfs.sh 取料, 再 sudo scripts/phone_prepare_linux.sh 解开\n", rootfs); return 3; }
    if (ph_cg_exists() && ph_cg_pids(NULL) > 0) { fprintf(stderr, "ds4-phone: 已在跑(先 stop)\n"); return 4; }
    if (ph_cg_exists()) ph_cg_kill();   /* 上次没收干净的空 cgroup */
    ph_net_down();                       /* 上次残留的 veth/规则(ip link del 顺带删了手机那头) */
    char data[PH_PATH + 96]; snprintf(data, sizeof data, "%s/data", ph_dir); mkdir(data, 0771);
    char logp[PH_PATH + 96]; snprintf(logp, sizeof logp, "%s/phone.log", ph_dir);
    const int log_fd = open(logp, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0) { fprintf(stderr, "ds4-phone: 写不了 %s\n", logp); return 1; }
    char dns[64]; ph_net_dns(dns, sizeof dns);

    if (!ph_cg_create(err, sizeof err)) { fprintf(stderr, "ds4-phone: cgroup: %s\n", err); return 1; }
    int c2p[2], p2c[2];
    if (pipe(c2p) || pipe(p2c)) { ph_cg_kill(); return 1; }
    /* 裸 clone 带 CLONE_NEWPID = "fork 出一个新 pid 命名空间的 1 号进程", 父进程自己留在宿主命名空间 ——
     * 10-13 实撞: 父进程 unshare(NEWPID) 再 fork, 之后父进程 fork 的 ip 命令也落进新命名空间, 按宿主 pid 找手机报 ESRCH。
     * 栈传 0 = fork 语义(两边都从这里返回); 这是 Linux 的约定, aarch64/x86_64 都一样。 */
    const pid_t pid = (pid_t)syscall(SYS_clone, CLONE_NEWPID | SIGCHLD, 0, 0, 0, 0);
    if (pid < 0) { ph_log("clone(NEWPID): %s", strerror(errno)); ph_cg_kill(); return 1; }
    if (pid == 0) {
        close(c2p[0]); close(p2c[1]);
        if (unshare(CLONE_NEWNS | CLONE_NEWNET | CLONE_NEWUTS | CLONE_NEWIPC)) { dprintf(log_fd, "unshare: %s\n", strerror(errno)); _exit(1); }
        if (write(c2p[1], "n", 1) != 1) _exit(1);
        char c; if (read(p2c[0], &c, 1) != 1 || c != 'g') _exit(1);   /* 等父进程把我放进 cgroup、把 eth0 塞进来 */
        close(c2p[1]); close(p2c[0]);
        if (unshare(CLONE_NEWCGROUP)) { dprintf(log_fd, "unshare(NEWCGROUP): %s\n", strerror(errno)); _exit(1); }   /* 要在进了 cgroup 之后: 命名空间的根 = 此刻所在的 cgroup */
        if (!ph_net_phone_side()) { dprintf(log_fd, "手机侧网络配置失败\n"); _exit(1); }
        ph_child_boot(log_fd, dns);
    }
    close(c2p[1]); close(p2c[0]);
    char c; bool ok = read(c2p[0], &c, 1) == 1 && c == 'n';
    if (ok && !ph_cg_join(pid)) { ph_log("把 init 放进 cgroup 失败: %s", strerror(errno)); ok = false; }
    if (ok && !ph_net_up(pid, err, sizeof err)) { ph_log("宿主侧网络: %s", err); ok = false; }
    if (ok && write(p2c[1], "g", 1) != 1) ok = false;
    if (!ok) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); ph_cg_kill(); ph_net_down(); return 1; }
    close(c2p[0]); close(p2c[1]);

    char sp[PH_PATH + 96]; snprintf(sp, sizeof sp, "%s/started", ph_dir);
    FILE *sf = fopen(sp, "w"); if (sf) { fprintf(sf, "%ld\n", (long)time(NULL)); fclose(sf); }
    ph_log("start: init pid %ld, 等 adbd(%s:%d) …", (long)pid, PH_IP_PHONE, PH_ADB_PORT);
    for (int i = 0; i < START_WAIT_S; i++) {
        int st; if (waitpid(pid, &st, WNOHANG) == pid) { ph_log("init 退出了(状态 %d), 看 phone.log 与 dmesg", st); ph_cg_kill(); ph_net_down(); return 1; }
        if (adb_listening()) { ph_log("ready: %d s, 内存 %ld MB", i, ph_cg_mem_mb(false)); printf("ready %d\n", i); return 0; }
        sleep(1);
    }
    ph_log("%d s 内 adbd 没起来; 手机留着跑(status 看进程数), 不行就 stop", START_WAIT_S);
    printf("timeout\n");
    return 5;
}

int main(int argc, char **argv) {
    if (argc != 2 || (strcmp(argv[1], "start") && strcmp(argv[1], "stop") && strcmp(argv[1], "status") && strcmp(argv[1], "dmesg"))) { fprintf(stderr, "用法: ds4-phone start|stop|status|dmesg\n"); return 2; }
    find_root();
    if (!strcmp(argv[1], "status")) return cmd_status();
    if (!strcmp(argv[1], "dmesg")) return cmd_dmesg();
    if (geteuid()) { fprintf(stderr, "ds4-phone: 要 root(sudo -n %s/ds4-phone %s; 免密规则由 scripts/phone_prepare_linux.sh 装)\n", ph_root, argv[1]); return 2; }
    mkdir(ph_dir, 0755);
    return strcmp(argv[1], "start") ? cmd_stop() : cmd_start();
}
