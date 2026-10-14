/* phone_mnt.c — 手机那头(新挂载命名空间里)的开机: 挂载表 → 设备节点 → pivot_root → exec /init。
 * 照 docker --privileged 给 redroid 的环境配最小一份(redroid 官方只在 docker 里跑, 它的 init 默认这些东西都在):
 *   /dev      tmpfs, 里面手工 mknod 一小把字符设备; binder 三兄弟是宿主 modprobe 出来的 misc 设备, 用宿主上的 rdev 号在这里再建一份节点
 *             (同一个驱动, 同一个 ipc; 一部手机不需要 binderfs 那种按命名空间隔离)
 *   /dev/pts  devpts newinstance(自己的 ptmx); /dev/shm /dev/mqueue
 *   /proc /sys 各自新挂(pid 命名空间 → /proc 只看得见手机里的进程; sysfs 要可写: ueventd 要往里写)
 *   /sys/fs/cgroup  cgroup2(cgroup 命名空间已 unshare, 手机看见的根就是 ds4-phone 那棵)
 *   /data     bind gguf/phone/data(手机的全部状态都在这)
 * 为什么 pivot_root(".", ".") 而不是 chroot: chroot 下宿主的挂载还在手机的视野里(/proc/mounts 里能看见, 路径也能走出去); pivot 后旧根被 detach, 彻底看不见。
 * 出错会怎样: 哪一步 mount 失败都直接 _exit(日志写进 phone.log 那条 fd); 最常见是 rootfs 不是 root 解的(属主不对, init 起来就 setuid 失败)。 */
#include "phone_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static int g_log = 2;
#define DIE(...) do { dprintf(g_log, "[boot] " __VA_ARGS__); dprintf(g_log, ": %s\n", strerror(errno)); _exit(1); } while (0)

static void mnt(const char *src, const char *dst, const char *type, unsigned long flags, const char *data) {
    mkdir(dst, 0755);
    if (mount(src, dst, type, flags, data)) DIE("mount %s on %s", type ? type : src, dst);
}

static void node(const char *path, mode_t mode, unsigned maj, unsigned min) {
    if (mknod(path, S_IFCHR | mode, makedev(maj, min))) DIE("mknod %s", path);
}

/* binder 三兄弟: 宿主内核(Ubuntu 6.17)的 binder_linux 编了 CONFIG_ANDROID_BINDERFS, 这时 devices= 参数不再造 /dev/binder 这种静态 misc 设备
 * (10-13 实撞: modprobe 后 /proc/misc 里没有 binder), 设备只能从 binderfs 来: 在手机自己的 ipc 命名空间里挂一份 binderfs,
 * 对 binder-control 发 BINDER_CTL_ADD 造三个设备, 再在 /dev 下放同名软链(redroid 的 init 按 /dev/binder 找)。
 * 结构与 ioctl 号照 linux/android/binderfs.h 抄一份: 不依赖宿主装没装内核头。 */
#define BINDERFS_MAX_NAME 255
struct binderfs_device { char name[BINDERFS_MAX_NAME + 1]; unsigned int major, minor; };
#define BINDER_CTL_ADD _IOWR('b', 1, struct binderfs_device)

static void binderfs(const char *dev) {
    char bf[PH_PATH + 160]; snprintf(bf, sizeof bf, "%s/binderfs", dev);
    mnt("binder", bf, "binder", MS_NOSUID | MS_NOEXEC, NULL);
    char ctl[PH_PATH + 192]; snprintf(ctl, sizeof ctl, "%s/binder-control", bf);
    const int fd = open(ctl, O_RDWR | O_CLOEXEC); if (fd < 0) DIE("open %s", ctl);
    static const char *const names[] = { "binder", "hwbinder", "vndbinder" };
    for (int i = 0; i < 3; i++) {
        struct binderfs_device d = {0}; snprintf(d.name, sizeof d.name, "%s", names[i]);
        if (ioctl(fd, BINDER_CTL_ADD, &d) && errno != EEXIST) DIE("BINDER_CTL_ADD %s", names[i]);   /* 模块带 devices= 参数时 binderfs 一挂上就自带这三个(10-13 实撞 EEXIST) */
        char link[PH_PATH + 192], target[64]; snprintf(link, sizeof link, "%s/%s", dev, names[i]); snprintf(target, sizeof target, "binderfs/%s", names[i]);
        if (symlink(target, link)) DIE("symlink %s", link);
    }
    close(fd);
}

void ph_child_boot(int log_fd, const char *dns) {
    g_log = log_fd;
    char R[PH_PATH + 64]; snprintf(R, sizeof R, "%s/rootfs", ph_dir);
    char p[PH_PATH + 192];
    if (sethostname("ds4-phone", 9)) DIE("sethostname");
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL)) DIE("make / private");
    if (mount(R, R, NULL, MS_BIND | MS_REC, NULL)) DIE("bind rootfs");

    snprintf(p, sizeof p, "%s/dev", R); mnt("tmpfs", p, "tmpfs", MS_NOSUID | MS_STRICTATIME, "mode=755,size=64m");
    char d[PH_PATH + 96]; snprintf(d, sizeof d, "%s/dev", R);
    snprintf(p, sizeof p, "%s/null", d);    node(p, 0666, 1, 3);
    snprintf(p, sizeof p, "%s/zero", d);    node(p, 0666, 1, 5);
    snprintf(p, sizeof p, "%s/full", d);    node(p, 0666, 1, 7);
    snprintf(p, sizeof p, "%s/random", d);  node(p, 0666, 1, 8);
    snprintf(p, sizeof p, "%s/urandom", d); node(p, 0666, 1, 9);
    snprintf(p, sizeof p, "%s/kmsg", d);    node(p, 0644, 1, 11);   /* Android init 的日志口 → 宿主 dmesg; 没有它 init 一声不吭地死 */
    snprintf(p, sizeof p, "%s/tty", d);     node(p, 0666, 5, 0);
    snprintf(p, sizeof p, "%s/console", d); node(p, 0600, 5, 1);
    snprintf(p, sizeof p, "%s/fuse", d);    node(p, 0666, 10, 229);  /* Android 11+ 的 /sdcard 是 FUSE 挂的 */
    binderfs(d);
    /* DMA heap: Codec2(软件 H.264 编码器所在)分配缓冲要它; 宿主的 /dev/dma_heap/system 是动态设备号, 照宿主的 rdev 建一份。没有就不建(录屏不可用, 别的照常) */
    struct stat hs;
    if (!stat("/dev/dma_heap/system", &hs) && S_ISCHR(hs.st_mode)) {
        snprintf(p, sizeof p, "%s/dma_heap", d); mkdir(p, 0755);
        snprintf(p, sizeof p, "%s/dma_heap/system", d); node(p, 0444, major(hs.st_rdev), minor(hs.st_rdev));
    } else dprintf(g_log, "[boot] 宿主没有 /dev/dma_heap/system: Codec2 起不来, 录屏流不可用\n");
    /* ashmem: 老 Chromium 分支的 WebView(淘宝闪购/饿了么的 UC 内核)只认 /dev/ashmem 的 ioctl, 没有就 CHECK 失败 SIGILL(10-14 实撞);
     * 主线 5.18 删了它, 宿主要装仓内 src/phone/ashmem 的树外模块(phone_prepare_linux.sh ⑤), 这里照宿主的 rdev 镜像一份。没有就只记日志 */
    if (!stat("/dev/ashmem", &hs) && S_ISCHR(hs.st_mode)) { snprintf(p, sizeof p, "%s/ashmem", d); node(p, 0666, major(hs.st_rdev), minor(hs.st_rdev)); }
    else dprintf(g_log, "[boot] 宿主没有 /dev/ashmem: 带老 WebView 内核的 app 会崩(sudo scripts/phone_prepare_linux.sh 装 ashmem 模块)\n");
    /* 我们的 rc 叠加(phone_rc.c): 每次开机重写, 免得 rootfs 里留一份旧的 */
    snprintf(p, sizeof p, "%s/vendor/etc/init/zz_ds4.rc", R);
    FILE *rc = fopen(p, "w"); if (!rc) DIE("write %s", p);
    fputs(ph_rc_text(), rc); fclose(rc); chmod(p, 0644);
    snprintf(p, sizeof p, "%s/pts", d);     mnt("devpts", p, "devpts", MS_NOSUID | MS_NOEXEC, "newinstance,ptmxmode=0666,mode=0620,gid=5");
    snprintf(p, sizeof p, "%s/ptmx", d);    if (symlink("pts/ptmx", p)) DIE("symlink ptmx");
    snprintf(p, sizeof p, "%s/shm", d);     mnt("tmpfs", p, "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, "mode=1777,size=256m");
    snprintf(p, sizeof p, "%s/mqueue", d);  mnt("mqueue", p, "mqueue", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
    /* 手机里的程序有时按 /dev/fd /dev/std* 找 */
    snprintf(p, sizeof p, "%s/fd", d); if (symlink("/proc/self/fd", p)) DIE("symlink fd");
    snprintf(p, sizeof p, "%s/stdin", d); if (symlink("/proc/self/fd/0", p)) DIE("symlink stdin");
    snprintf(p, sizeof p, "%s/stdout", d); if (symlink("/proc/self/fd/1", p)) DIE("symlink stdout");
    snprintf(p, sizeof p, "%s/stderr", d); if (symlink("/proc/self/fd/2", p)) DIE("symlink stderr");

    snprintf(p, sizeof p, "%s/proc", R); mnt("proc", p, "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
    snprintf(p, sizeof p, "%s/sys", R);  mnt("sysfs", p, "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
    snprintf(p, sizeof p, "%s/sys/fs/cgroup", R); mnt("cgroup2", p, "cgroup2", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
    char data[PH_PATH + 96]; snprintf(data, sizeof data, "%s/data", ph_dir);
    snprintf(p, sizeof p, "%s/data", R); mkdir(p, 0771);
    if (mount(data, p, NULL, MS_BIND, NULL)) DIE("bind /data");

    if (chdir(R)) DIE("chdir rootfs");
    if (syscall(SYS_pivot_root, ".", ".")) DIE("pivot_root");   /* 新根和旧根同一处: 旧根压在下面, 下一行 detach 掉 */
    if (umount2(".", MNT_DETACH)) DIE("umount old root");
    if (chdir("/")) DIE("chdir /");

    /* stdin 关进 /dev/null, stdout/stderr 进 phone.log; 其余 fd 不带过去 */
    const int nul = open("/dev/null", O_RDONLY); if (nul >= 0) { dup2(nul, 0); close(nul); }
    dup2(g_log, 1); dup2(g_log, 2); if (g_log > 2) close(g_log);
    for (int fd = 3; fd < 64; fd++) close(fd);

    char w[48], h[48], dpi[48], fps[48], dns1[96];
    snprintf(w, sizeof w, "androidboot.redroid_width=%d", PH_WIDTH);
    snprintf(h, sizeof h, "androidboot.redroid_height=%d", PH_HEIGHT);
    snprintf(dpi, sizeof dpi, "androidboot.redroid_dpi=%d", PH_DPI);
    snprintf(fps, sizeof fps, "androidboot.redroid_fps=%d", PH_FPS);
    snprintf(dns1, sizeof dns1, "androidboot.redroid_net_dns1=%s", dns);
    /* 前两项照镜像的 Entrypoint(gguf/phone/image.json); use_memfd: 宿主内核没有 ashmem(5.18 起删), 不开这个 init 起不来 */
    char *const argv[] = { "/init", "qemu=1", "androidboot.hardware=redroid", "androidboot.use_memfd=true", "androidboot.redroid_gpu_mode=guest",
                           w, h, dpi, fps, "androidboot.redroid_net_ndns=1", dns1, NULL };
    char *const envp[] = { "PATH=/system/bin:/system/xbin:/vendor/bin", NULL };
    execve("/init", argv, envp);
    DIE("execve /init");
}
