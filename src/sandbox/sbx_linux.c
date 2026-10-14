/* sbx_linux.c — Linux 沙盒上锁: no_new_privs → Landlock → seccomp。总述见 asst_internal.h, 起收杀见 sbx_run.c。
 *
 * Landlock(5.13+, 非特权可用; spark 6.17 = ABI 7): 文件按"目录层级"白名单, 软链/.. 都按真实层级判, 白名单外 EACCES。
 *   ABI≥4 再管 TCP: 不加任何端口规则 = connect/bind 全拒; ABI≥6 再管"信号与抽象 unix socket 不许出域": 工具杀不了 ds4-server, 连不上 X11/dbus 的抽象口。
 *   Landlock 也自带 ptrace 限制: 域内进程 ptrace 不了域外进程(读不到别的进程的 /proc/<pid>/environ、mem)。
 * seccomp: Landlock 管不到 UDP、具名 unix socket(connect 一个 socket 文件不算"写文件")、io_uring(绕过 seccomp 的 socket 过滤)、命名空间, 这里一律 EPERM。
 *   socket() 整个拒(含 AF_UNIX): 具名 unix socket 是最危险的口子 —— /var/run/docker.sock 在 docker 组里等于 root。socketpair 留着(管道用)。
 *   clone 带 CLONE_NEW* 拒, clone3 回 ENOSYS(参数是结构体指针 BPF 看不见; glibc 见 ENOSYS 自动退回 clone, 再被上一条查)。
 * 为什么不用 bwrap/用户命名空间: spark 的 Ubuntu 24.04 开着 apparmor_restrict_unprivileged_userns=1, 非特权建不了命名空间; Landlock 不受它管。
 * 出错会怎样: 内核不带 Landlock(没编进或 lsm= 里没有)→ 上锁失败 → 工具拒绝执行, 审计记 "sandbox lock failed (landlock+seccomp): …"。 */
#ifdef __linux__
#include "../assistant/asst_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

const char *asst_sbx_kind(void) { return "landlock+seccomp"; }

/* 自己定义 Landlock 的 ABI 常量与结构: 构建机内核头可能比运行机旧(24.04 的头是 6.8, 没有 ABI6 的 scoped 字段), 常量是内核 ABI, 永远不变 */
#define LL_CREATE_VERSION 1u
#define LL_RULE_PATH_BENEATH 1
#define LL_FS_EXECUTE (1ull << 0)
#define LL_FS_WRITE_FILE (1ull << 1)
#define LL_FS_READ_FILE (1ull << 2)
#define LL_FS_READ_DIR (1ull << 3)
#define LL_FS_REFER (1ull << 13)
#define LL_FS_TRUNCATE (1ull << 14)
#define LL_FS_IOCTL_DEV (1ull << 15)
#define LL_NET_ALL ((1ull << 0) | (1ull << 1))   /* BIND_TCP | CONNECT_TCP */
#define LL_SCOPE_ALL ((1ull << 0) | (1ull << 1)) /* ABSTRACT_UNIX_SOCKET | SIGNAL */
#ifndef __NR_landlock_create_ruleset
#define __NR_landlock_create_ruleset 444
#define __NR_landlock_add_rule 445
#define __NR_landlock_restrict_self 446
#endif
struct ll_ruleset_attr { uint64_t fs, net, scoped; };
struct ll_path_beneath { uint64_t allowed; int32_t parent_fd; } __attribute__((packed));

/* 这个 ABI 认得的全部文件权限位: 全部"接管"(handled), 再按根逐个放开; 没接管的位内核不管 = 放行, 所以必须接管满 */
static uint64_t fs_all(int abi) {
    uint64_t m = (1ull << 13) - 1;   /* v1: EXECUTE … MAKE_SYM 共 13 位 */
    if (abi >= 2) m |= LL_FS_REFER;
    if (abi >= 3) m |= LL_FS_TRUNCATE;
    if (abi >= 5) m |= LL_FS_IOCTL_DEV;
    return m;
}
#define LL_FILE_ONLY (LL_FS_EXECUTE | LL_FS_WRITE_FILE | LL_FS_READ_FILE | LL_FS_TRUNCATE | LL_FS_IOCTL_DEV)   /* 规则落在单个文件上只能带这些位, 带目录位 EINVAL */

static bool add_root(int rs, const char *path, uint64_t allowed, char *err, size_t errn) {
    const int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) { snprintf(err, errn, "open %s: %s", path, strerror(errno)); return false; }
    struct stat st;
    if (fstat(fd, &st) == 0 && !S_ISDIR(st.st_mode)) allowed &= LL_FILE_ONLY;
    struct ll_path_beneath pb = { allowed, fd };
    const long r = syscall(__NR_landlock_add_rule, rs, LL_RULE_PATH_BENEATH, &pb, 0);
    const int e = errno;
    close(fd);
    if (r) { snprintf(err, errn, "landlock_add_rule %s: %s", path, strerror(e)); return false; }
    return true;
}

/* bash 要跑起来必须读的系统目录: 都是装好的软件与配置, 没有用户数据; 家目录、/tmp、/run、/var 一概不给 */
static const char *const exec_ro[] = { "/usr", "/bin", "/sbin", "/lib", "/lib64", "/etc", "/opt", "/proc", "/dev/zero", "/dev/urandom", "/dev/random", NULL };

static bool landlock_lock(const asst_sbx_spec *sp, char *err, size_t errn) {
    const long abi = syscall(__NR_landlock_create_ruleset, NULL, 0, LL_CREATE_VERSION);
    if (abi < 1) { snprintf(err, errn, "kernel has no landlock (%s)", strerror(errno)); return false; }
    const uint64_t all = fs_all((int)abi);
    struct ll_ruleset_attr at = { all, abi >= 4 ? LL_NET_ALL : 0, abi >= 6 ? LL_SCOPE_ALL : 0 };
    const size_t asz = abi >= 6 ? 24 : abi >= 4 ? 16 : 8;   /* 老内核只认前缀长度的结构, 多给字节 E2BIG */
    const int rs = (int)syscall(__NR_landlock_create_ruleset, &at, asz, 0);
    if (rs < 0) { snprintf(err, errn, "landlock_create_ruleset: %s", strerror(errno)); return false; }
    const uint64_t ro = LL_FS_READ_FILE | LL_FS_READ_DIR | (sp->exec ? LL_FS_EXECUTE : 0);
    bool ok = true;
    for (int i = 0; ok && i < sp->nro; i++) ok = add_root(rs, sp->ro[i], ro, err, errn);
    for (int i = 0; ok && i < sp->nrw; i++) ok = add_root(rs, sp->rw[i], all & ~(sp->exec ? 0 : LL_FS_EXECUTE), err, errn);
    if (ok && sp->exec) {
        for (int i = 0; ok && exec_ro[i]; i++) if (!access(exec_ro[i], F_OK)) ok = add_root(rs, exec_ro[i], LL_FS_READ_FILE | LL_FS_READ_DIR | LL_FS_EXECUTE, err, errn);
        if (ok) ok = add_root(rs, "/dev/null", LL_FS_READ_FILE | LL_FS_WRITE_FILE, err, errn);
    }
    if (ok && syscall(__NR_landlock_restrict_self, rs, 0)) { snprintf(err, errn, "landlock_restrict_self: %s", strerror(errno)); ok = false; }
    close(rs);
    return ok;
}

#if defined(__aarch64__)
#define SBX_AUDIT_ARCH AUDIT_ARCH_AARCH64
#elif defined(__x86_64__)
#define SBX_AUDIT_ARCH AUDIT_ARCH_X86_64
#endif

#define CLONE_NS_MASK 0x7E020080u   /* NEWNS|NEWCGROUP|NEWUTS|NEWIPC|NEWUSER|NEWPID|NEWNET|NEWTIME */

static bool seccomp_lock(char *err, size_t errn) {
#ifndef SBX_AUDIT_ARCH
    snprintf(err, errn, "seccomp filter not written for this CPU arch"); return false;
#else
    static const int deny[] = {
        __NR_socket, __NR_ptrace, __NR_process_vm_readv, __NR_process_vm_writev, __NR_mount, __NR_umount2, __NR_pivot_root, __NR_chroot,
        __NR_unshare, __NR_setns, __NR_bpf, __NR_perf_event_open, __NR_keyctl, __NR_add_key, __NR_request_key, __NR_userfaultfd,
        __NR_kexec_load, __NR_init_module, __NR_finit_module, __NR_delete_module, __NR_swapon, __NR_swapoff, __NR_reboot,
        __NR_io_uring_setup, __NR_io_uring_enter, __NR_io_uring_register,
#ifdef __NR_open_tree
        __NR_open_tree, __NR_move_mount, __NR_fsopen, __NR_fsconfig, __NR_fsmount, __NR_fspick,
#endif
#ifdef __NR_mount_setattr
        __NR_mount_setattr,
#endif
    };
    enum { ND = sizeof deny / sizeof deny[0] };
    struct sock_filter f[2 * ND + 16]; int n = 0;   /* 每条拒绝 2 条指令(比较 + 返回), 外加架构头 4 条与 clone/clone3 尾巴 7 条 */
    f[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch));
    f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SBX_AUDIT_ARCH, 1, 0);
    f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);   /* 换架构的调用(x86 上 int 0x80)= 绕过, 直接杀 */
    f[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr));
    for (int i = 0; i < ND; i++) {
        f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned)deny[i], 0, 1);
        f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
    }
    f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_clone3, 0, 1);
    f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS);
    f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_clone, 0, 3);
    f[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0]));   /* 小端: 低 32 位就是 flags 全部 */
    f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, CLONE_NS_MASK, 0, 1);
    f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
    f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    struct sock_fprog prog = { (unsigned short)n, f };
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog)) { snprintf(err, errn, "seccomp: %s", strerror(errno)); return false; }
    return true;
#endif
}

bool asst_sbx_lock(const asst_sbx_spec *sp, char *err, size_t errn) {
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) { snprintf(err, errn, "no_new_privs: %s", strerror(errno)); return false; }   /* 没它 setuid 程序能把锁甩掉; 有它 sudo 直接失效 */
    return landlock_lock(sp, err, errn) && seccomp_lock(err, errn);
}
#endif
