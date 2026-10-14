/* phone_internal.h — ds4-phone: 云手机的运行时(10-13, 方案稿 docs/assistant_phone.md §3)。
 * 一句话: 把 redroid 的 Android 根文件系统(gguf/phone/rootfs/)当一棵进程树拉起来 —— 自己的 pid/挂载/网络/uts/ipc/cgroup 命名空间,
 * 内存硬顶 1 GB, 一根 veth 出公网(内网隔断), 没有 docker、没有守护进程: start 把 /init 拉起来就退出, stop 往 cgroup.kill 写 1。
 * 为什么要 root: Android 的 init 必须是 uid 0(mknod、mount、setuid 到几十个 app uid), cgroup/命名空间/NAT 也都要 root。
 *   主进程 ds4-train 不提权, 通过 sudoers 里一行只放行本二进制的免密规则调它(scripts/phone_prepare_linux.sh 装)。
 *   所以本程序★不从 argv 取任何路径★: 一切路径从自己的可执行文件位置推出(仓库根/gguf/phone/…), argv 只认 start|stop|status。
 * 布局(仓库根 = 本二进制所在目录):
 *   gguf/phone/rootfs/   解开的镜像层(phone_prepare_linux.sh 用 root 解, 属主/xattr 原样)
 *   gguf/phone/data/     手机的 /data(装的 app、账号都在这; 关机不删, 想要"恢复出厂"就删这个目录)
 *   gguf/phone/phone.log /init 的 stdout/stderr(Android 自己的日志走 logcat, 在手机里)
 *   gguf/phone/started   起跑时刻(epoch 秒), status 算 uptime 用
 * 出错会怎样: 不是 root → 退出码 2 并提示用 sudo; rootfs 不在 → 退出码 3 并给出取料/准备脚本; 已在跑再 start → 退出码 4。 */
#ifndef DS4_PHONE_INTERNAL_H
#define DS4_PHONE_INTERNAL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define PH_PATH 1024
#define PH_CGROUP "/sys/fs/cgroup/ds4-phone"
/* 内存硬顶按起手机那一刻宿主的 MemAvailable 定: cap = min(PH_MEM_MAX_MB, MemAvailable − PH_MEM_HEADROOM_MB), 低于 PH_MEM_MIN_MB 就不起(phone_cg.c)。
 * 10-14: 写死 6 GB 时 spark 带着模型只剩 8.4 GB 可用, 主进程的门(需 cap + 3 GB)直接拒起; 写死 3 GB 微信又抖动 —— 顶只能按当时的账算, 状态接口报真实的顶。 */
#define PH_MEM_HEADROOM_MB 3072     /* 起手机后宿主至少还剩这么多(和 asst_internal.h ASST_PHONE_MIN_AVAIL_MB 同一个数, 两个程序各写一份) */
#define PH_MEM_MIN_MB 2048          /* 低于这个顶没法跑一个像样的 app(淘宝闪购 1 GB 顶实撞 OOM, 10-13) */
#define PH_MEM_MAX_MB 6144          /* 硬顶: 超了手机自己被 OOM 杀, 主进程与模型子进程不受影响。裁剪后空载 464 MB; 淘宝闪购一个 app 开 4 个进程 RSS ~1.4 GB,
                                     * 10-14 微信: PSS 只 1.3 GB 但 cgroup 把 apk/oat/数据的页缓存也算在内, 3 GB 顶撞 memory.events max 3358 次(没 oom_kill, 是反复回收抖动)
                                     * ⇒ 主线程 ANR("Input dispatching timed out"/广播超时), 页面上点发送没反应、弹"无响应"。
                                     * 10-13: 2 GB 顶下 memory.events max 撞 2222 次、screencap 回 0 字节; 3 GB 是淘宝闪购 + 系统 + 页缓存的账, 微信不够。 */
#define PH_PIDS_MAX 2048
#define PH_CPU_CORES 4
#define PH_VETH_HOST "ds4ph0"       /* 宿主这头; 手机那头叫 eth0(redroid 的 ipconfigstore 认这个名) */
#define PH_IP_HOST "172.31.250.1"
#define PH_IP_PHONE "172.31.250.2"
#define PH_SUBNET "172.31.250.0/30"
#define PH_ADB_PORT 5555
#define PH_WIDTH 720                /* 帧缓冲与截图都是 1080p 的 1/2.25; redroid 的默认值也是这三项 */
#define PH_HEIGHT 1280
#define PH_DPI 320
#define PH_FPS 15                   /* 软渲染; redroid 不开 GPU 时的默认 */
#define PH_STOP_WAIT_S 8

extern char ph_root[PH_PATH];       /* 仓库根(本二进制所在目录) */
extern char ph_dir[PH_PATH + 32];   /* 仓库根/gguf/phone */

/* phone_main.c */
void ph_log(const char *fmt, ...);
int ph_run(char *const argv[], bool quiet);    /* fork+exec+wait, 返回退出码(-1 = 起不来); quiet = 失败不记日志 */

/* phone_cg.c: cgroup v2 子树 */
bool ph_cg_exists(void);
bool ph_cg_create(char *err, size_t errn);
bool ph_cg_join(pid_t pid);                    /* 把这个进程放进去(手机的 init; 父进程自己不进) */
int  ph_cg_pids(pid_t *first);                 /* 里面的进程数; first = 最小 pid(= 手机的 init) */
long ph_cg_mem_mb(bool peak);
long ph_cg_cap_mb(void);            /* 当前 cgroup 的 memory.max(MB); 没建 = -1 */
bool ph_cg_kill(void);                         /* cgroup.kill → 等到空 → rmdir */

/* phone_net.c: 宿主侧 veth + NAT + 内网隔断; 手机侧 eth0 配置 */
bool ph_net_up(pid_t phone_pid, char *err, size_t errn);
void ph_net_down(void);
bool ph_net_phone_side(void);                  /* 在手机的网络命名空间里(pivot_root 前)调: lo/eth0/默认路由 */
void ph_net_dns(char *out, size_t n);          /* 宿主上游 DNS(绕开 systemd-resolved 的 127.0.0.53); 查不到给 8.8.8.8(redroid 自己的默认) */

/* phone_rc.c: 叠给 Android init 的 rc 文本(写进 rootfs/vendor/etc/init/zz_ds4.rc) */
const char *ph_rc_text(void);

/* phone_mnt.c: 手机那头的挂载表 + 设备节点 + pivot_root + exec /init(不返回) */
void ph_child_boot(int log_fd, const char *dns) __attribute__((noreturn));

#endif
