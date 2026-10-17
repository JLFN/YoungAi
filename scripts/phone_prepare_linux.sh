#!/bin/bash
# phone_prepare_linux.sh — 云手机的一次性准备(Linux 主机, 要 root; 幂等, 重跑无害)。10-13, 方案稿 docs/assistant_phone.md §2。
#
# 怎么用:  cd ~/ds4-main && make ds4-phone && sudo scripts/phone_prepare_linux.sh
# 做四件事(每件为什么):
#   ① modprobe binder_linux devices="binder,hwbinder,vndbinder"  —— Android 的进程间通信全靠 binder, 宿主内核自带模块但默认不加载;
#      写进 /etc/modules-load.d + /etc/modprobe.d 让重启后还在。
#   ② 把 gguf/phone/layer.tar.gz(phone_fetch_rootfs.sh 拉的那一层)解到 gguf/phone/rootfs/ —— 必须 root 解: 文件属主(system/radio/…)、
#      setuid 位、security.capability xattr 都要原样, 普通用户解出来的 rootfs 起来 init 一 setuid 就死。校验 sha256 后才解。
#   ③ sudoers 一行: 让跑工作台的那个用户免密执行 ./ds4-phone(只这一个二进制; 它自己只认 start|stop|status, 不从参数取路径)。
#   ④ 开 ip_forward(手机经宿主 NAT 出公网)并写 sysctl.d 持久化。
# 不装任何包、不起任何服务。
# 出错会怎样: 不是 root → 退 2; layer 没拉 → 退 3 并指向取料脚本; sha256 不对 → 退 4 不解。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[ "$(id -u)" = 0 ] || { echo "要 root: sudo $0"; exit 2; }
OWNER="${SUDO_USER:-}"; [ -n "$OWNER" ] || { echo "拿不到 SUDO_USER(请用 sudo 跑, 不要 su 进 root 再跑)"; exit 2; }
PH="$ROOT/gguf/phone"
BIN="$ROOT/ds4-phone"
[ -x "$BIN" ] || { echo "先 make ds4-phone($BIN 不在)"; exit 3; }

# ① binder
# 新内核(Ubuntu 6.17)的 binder_linux 带 binderfs, 加载后★不会★出现 /dev/binder —— 设备由 ds4-phone 在手机自己的命名空间里从 binderfs 造(src/sandbox/phone/phone_mnt.c);
# 这里只要模块在、/proc/filesystems 里有 binder 就算好。devices= 只对不带 binderfs 的老内核有意义, 带上无害。
lsmod | grep -q '^binder_linux' || modprobe binder_linux devices="binder,hwbinder,vndbinder" || { echo "modprobe binder_linux 失败: 内核没这个模块(apt install linux-modules-extra-$(uname -r))"; exit 1; }
echo 'binder_linux' > /etc/modules-load.d/ds4-phone.conf
echo 'options binder_linux devices=binder,hwbinder,vndbinder' > /etc/modprobe.d/ds4-phone.conf
grep -q binder /proc/filesystems && echo "binder: 模块已载, binderfs 可用" || { echo "模块载了但 /proc/filesystems 里没有 binder"; exit 1; }

# ② rootfs
[ -f "$PH/layer.tar.gz" ] || { echo "$PH/layer.tar.gz 不在: 先在能上 Docker Hub 的机器跑 scripts/phone_fetch_rootfs.sh push"; exit 3; }
want="$(cut -c8- "$PH/layer.sha256" 2>/dev/null)"
have="$(sha256sum "$PH/layer.tar.gz" | cut -d' ' -f1)"
[ -n "$want" ] && [ "$want" = "$have" ] || { echo "layer.tar.gz sha256 对不上(要 $want, 是 $have), 不解"; exit 4; }
if [ -x "$PH/rootfs/init" ] && [ "$(cat "$PH/rootfs/.layer.sha256" 2>/dev/null)" = "$want" ]; then
    echo "rootfs 已是这一层, 不重解"
else
    rm -rf "$PH/rootfs.tmp"; mkdir -p "$PH/rootfs.tmp"
    echo "解 rootfs(640 MB gzip → ~1.5 GB) …"
    tar -xzf "$PH/layer.tar.gz" -C "$PH/rootfs.tmp" --numeric-owner --xattrs --xattrs-include='*' || { echo "解包失败"; rm -rf "$PH/rootfs.tmp"; exit 1; }
    echo "$want" > "$PH/rootfs.tmp/.layer.sha256"
    rm -rf "$PH/rootfs.old"; [ -d "$PH/rootfs" ] && mv "$PH/rootfs" "$PH/rootfs.old"
    mv "$PH/rootfs.tmp" "$PH/rootfs" && rm -rf "$PH/rootfs.old"
fi
mkdir -p "$PH/data"; chown root:root "$PH/data"; chmod 771 "$PH/data"
touch "$PH/phone.log"; chown "$OWNER" "$PH/phone.log"   # ds4-phone 以 root 写, 工作台用户要能读
du -sh "$PH/rootfs" | cut -f1 | sed 's/^/rootfs 体积: /'

# ③ sudoers
echo "$OWNER ALL=(root) NOPASSWD: $BIN" > /etc/sudoers.d/ds4-phone
chmod 440 /etc/sudoers.d/ds4-phone
visudo -cf /etc/sudoers.d/ds4-phone >/dev/null || { rm -f /etc/sudoers.d/ds4-phone; echo "sudoers 行没过 visudo 校验(路径里有空格?), 已撤"; exit 1; }

# ④ 转发
sysctl -q -w net.ipv4.ip_forward=1
echo 'net.ipv4.ip_forward=1' > /etc/sysctl.d/90-ds4-phone.conf

# ⑤ ashmem(10-14): 淘宝闪购/饿了么这类带老 Chromium 内核的 app 只认 /dev/ashmem, 主线 5.18 删了它 ⇒ 装仓内移植的树外模块(src/sandbox/phone/ashmem, 先 make 编出 .ko)。
#    装进 /lib/modules/<ver>/extra + depmod, 和 binder 一起写进 modules-load.d 让重启后还在。没编 .ko 就跳过并提示(手机照常起, 只是那类 app 会崩)。
KO="$ROOT/src/sandbox/phone/ashmem/ashmem_linux.ko"
if [ -f "$KO" ]; then
    # Secure Boot 开着时内核只认签过名的模块(insmod 报 "Key was rejected by service", 10-14 spark 实撞)。签名用 Ubuntu DKMS 同一把 MOK
    # (/var/lib/shim-signed/mok/MOK.priv), 它必须已经在固件里注册(注册要重启一次并在开机蓝屏 MOK 管理器里确认, 只能在机器跟前做):
    #   sudo update-secureboot-policy --new-key     # 造钥匙(已有就跳过)
    #   sudo update-secureboot-policy --enroll-key  # 设一次性密码 → 重启 → 蓝屏 Enroll MOK → Continue → Yes → 输那个密码 → 重启
    if mokutil --sb-state 2>/dev/null | grep -q enabled; then
        MOK=/var/lib/shim-signed/mok
        # 注册与否按证书指纹查固件里的 MokListRT: `mokutil --test-key` 以 root 跑时查的是内核钥匙环, 对已注册的钥匙也答 "not enrolled"(10-17 实撞, finish 白跑两趟)
        FP=$(openssl x509 -inform DER -in "$MOK/MOK.der" -noout -fingerprint -sha1 2>/dev/null | cut -d= -f2 | tr 'A-F' 'a-f')
        ENROLLED=$(mokutil --list-enrolled 2>/dev/null | tr 'A-F' 'a-f')   # 存变量再比: mokutil 退出码非零 + 本脚本的 pipefail 会让 grep 管道误判
        if [ -f "$MOK/MOK.priv" ] && [ -n "$FP" ] && [[ $ENROLLED == *"$FP"* ]]; then
            kmodsign sha512 "$MOK/MOK.priv" "$MOK/MOK.der" "$KO" || { echo "kmodsign 失败"; exit 1; }
            echo "ashmem: 已用 $MOK/MOK.der 签名"
        else
            echo "Secure Boot 开着且 MOK 没注册: 先 sudo update-secureboot-policy --new-key && sudo update-secureboot-policy --enroll-key, 重启在蓝屏里确认, 再跑本脚本"; exit 5
        fi
    fi
    install -D -m 644 "$KO" "/lib/modules/$(uname -r)/extra/ashmem_linux.ko" && depmod -a
    lsmod | grep -q '^ashmem_linux' || modprobe ashmem_linux || { echo "modprobe ashmem_linux 失败(dmesg | tail 看原因)"; exit 1; }
    grep -q '^ashmem_linux$' /etc/modules-load.d/ds4-phone.conf || echo 'ashmem_linux' >> /etc/modules-load.d/ds4-phone.conf
    [ -c /dev/ashmem ] && echo "ashmem: 模块已载, /dev/ashmem 在" || { echo "模块载了但 /dev/ashmem 没出现"; exit 1; }
else
    echo "没有 $KO: 先 make -C src/sandbox/phone/ashmem(要 linux-headers-$(uname -r)); 不装 ashmem 的话带老 WebView 内核的 app 会崩"
fi

echo "准备完成。试: sudo -n $BIN status"
