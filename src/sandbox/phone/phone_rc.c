/* phone_rc.c — 我们叠给 Android init 的那一份 rc(写进 rootfs 的 /vendor/etc/init/zz_ds4.rc; 文件名 zz_ 开头 = 同一触发器里排在 redroid 自己的
 * redroid.common.rc 之后, 后设的属性赢)。内容写死在这里而不是读一个文件: 本程序以 root 跑, 从用户可写的文件里取内容再写进 rootfs 没必要多一道口。
 * 每一条为什么:
 *   debug.stagefright.ccodec=1  redroid 默认关掉 Codec2("DMA Heap required"), 而 Android 13 的软件 H.264 编码器只有 Codec2 那一份(OMX 的早删了),
 *                               关着就 "unable to create video/avc codec instance"(10-13 实撞), 录屏流起不来。DMA heap 我们给(phone_mnt.c 的 /dev/dma_heap/system)。
 *                               放在 on init(晚于 redroid 的 early-init), mediaserver 起来时读到的已是 1。
 *   停服务: 手机才需要、这里没硬件的 —— 蓝牙模拟 HAL 开机就 SIGABRT 三连(logcat 10-13), 留着只是反复崩再拉起, 白吃 CPU 与内存。
 *   停用自带 app: pm disable-user 落 /data, 第二次开机起是空操作。命令行按包逐条拼(不用 shell 变量: init 把 $ 当自己的属性展开, 10-13 实撞只禁了 1 个包)。
 * 出错会怎样: 服务名写错 init 只记一行 warning, 不影响开机 —— 所以每个名字都要在 rootfs 的 rc 里 grep 到才许写进来。 */
#include "phone_internal.h"

/* 开机完成后停掉没硬件对应的 HAL/守护(名字都在 rootfs 的 vendor/etc/init 与 system/etc/init 里 grep 过, 10-13):
 * 蓝牙模拟 HAL、电话 rild、wifi HAL + wificond、DRM/CAS、相机、perfetto 三件、incidentd、mdnsd、usbd、redroid 的 VNC/uinput */
static const char *const STOP[] = { "vendor.bluetooth-1-1", "vendor.ril-daemon", "vendor.wifi_hal_legacy", "wificond", "vendor.cas-hal-1-2", "vendor.drm-clearkey-hal-1-4", "drm",
                                    "cameraserver", "traced", "traced_probes", "traced_perf", "incidentd", "mdnsd", "usbd", "vendor.vncserver", "vendor.uinputd", NULL };
/* 停用的自带 app: 相机/日历/闹钟/音乐/打印/蓝牙/壁纸/彩蛋/屏保/首次开机向导这些, 每个开着都是一个常驻或随时被拉起的进程(10-13 PSS 表: deskclock 12 MB、traceur 12 MB、camera2 8 MB …)。
 * 留着的: 设置、桌面、系统界面、文件、图库、联系人、输入法、安装器、权限、WebView 与 WebView Shell(手机里唯一的浏览器)、电话/电信(砍了系统不稳)。 */
static const char *const DISABLE[] = { "com.android.camera2", "com.android.cameraextensions", "com.android.calendar", "com.android.deskclock", "com.android.traceur", "com.android.quicksearchbox",
                                       "com.android.printspooler", "com.android.bips", "com.android.printservice.recommendation", "com.android.dynsystem", "com.android.smspush",
                                       "com.android.bluetooth", "com.android.bluetoothmidiservice", "com.android.managedprovisioning", "com.android.onetimeinitializer",
                                       "com.android.dreams.basic", "com.android.dreams.phototable", "com.android.egg", "com.android.music", "com.android.musicfx",
                                       "com.android.wallpaper.livepicker", "com.android.wallpapercropper", "com.android.hotspot2.osulogin", "com.android.nearby.halfsheet",
                                       "com.android.companiondevicemanager", "com.android.mtp", "com.android.se", "com.android.simappdialog", NULL };

const char *ph_rc_text(void) {
    static char buf[8192];
    if (buf[0]) return buf;
    size_t L = 0;
#define PUT(...) L += (size_t)snprintf(buf + L, sizeof buf - L, __VA_ARGS__)
    PUT("# ds4-phone 叠加(src/sandbox/phone/phone_rc.c 生成, 每次 start 重写; 改这里没用)\n");
    PUT("on init\n    setprop debug.stagefright.ccodec 1\n\n");
    PUT("on property:sys.boot_completed=1\n");
    for (int i = 0; STOP[i]; i++) PUT("    stop %s\n", STOP[i]);
    PUT("    exec_background - shell shell -- /system/bin/sh -c \"");
    for (int i = 0; DISABLE[i]; i++) PUT("pm disable-user --user 0 %s >/dev/null 2>&1;", DISABLE[i]);
    /* 屏幕永不灭: 灭了 screencap 是全黑、录屏流停帧(10-13 实撞: 页面一片黑); 这部手机没电池, 没有省电的理由 */
    PUT("settings put system screen_off_timeout 2147483647 >/dev/null 2>&1;svc power stayon true >/dev/null 2>&1;input keyevent 224 >/dev/null 2>&1;");
    PUT("true\"\n\n");
#undef PUT
    return buf;
}
