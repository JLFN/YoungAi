#!/bin/bash
# phone_ashmem_mok_spark.sh — 在 spark 本机(接着显示器)一次性装好 ashmem 内核模块, 让云手机里带老 WebView 内核的 app(12306/饿了么)不再自杀。
# 10-17。Mac 远程版是 phone_ashmem_mok_mac.sh(同一套步骤, 只是经 ssh); 背景见 docs/assistant_phone.md §11.2 与 fable5 "云手机装 12306"。
#
# 在 spark 的终端里按顺序跑三步(中间一次重启, 开机蓝屏按几下键):
#   cd ~/ds4-main
#   scripts/phone_ashmem_mok_spark.sh status   # 看现状(不改任何东西)
#   scripts/phone_ashmem_mok_spark.sh enroll   # ① 提交 MOK 注册: 要 sudo 密码; 让你设一个一次性密码(8~16 位, 输两遍不回显); 最后问你要不要现在重启
#       ↓ 重启后的蓝屏 "Perform MOK management"(只停约 10 s, 没按键就进系统了, 注册作废): Enroll MOK → Continue → Yes → 输那个一次性密码 → Reboot
#   scripts/phone_ashmem_mok_spark.sh finish   # ② 核对注册成功 → 签名+装模块+开机自动加载(仓内 phone_prepare_linux.sh ⑤) → 重启云手机 → 开 12306 看 60 s 死不死
#   scripts/phone_ashmem_mok_spark.sh test     # 只跑 finish 的最后一段(模块已装、工作台刚起来时用, 不要 sudo)
#
# 为什么非得重启一次: MOK 钥匙是注册进固件的, 只能在开机时由 shim 的蓝屏菜单确认, 开着的系统做不了; 以后不用再重启也不用再按键(模块写进 modules-load.d)。
# 出错会怎样: enroll 末尾 "待注册清单" 是空的 = 没提交成功, 别重启, 看报错重跑;
#            finish 说 "MOK 没注册" = 蓝屏那步没做成, 再 enroll 一遍;
#            finish 说 "Key was rejected" = 签名用的钥匙和注册的不是同一把, 看 /var/lib/shim-signed/mok/ 是不是被动过;
#            finish 说 "工作台没在跑" = 模块已装好, 只是手机那段测不了, 先按平时方式起工作台(train_ui_spark.sh start)再跑一次 finish。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MOK=/var/lib/shim-signed/mok                # Ubuntu DKMS 用的那把 MOK, phone_prepare_linux.sh ⑤ 签名也用它
KO="$ROOT/src/sandbox/phone/ashmem/ashmem_linux.ko"
PORT=8000                                   # 工作台主进程 ds4-train

# 注册与否按证书指纹查固件里的 MokListRT(root 与否答案一致)。不用 `mokutil --test-key`: 它以 root 跑时查的是内核钥匙环, 已注册的钥匙也答 "not enrolled"(10-17 实撞)
mok_fp() { openssl x509 -inform DER -in "$MOK/MOK.der" -noout -fingerprint -sha1 2>/dev/null | cut -d= -f2 | tr 'A-F' 'a-f'; }
mok_enrolled() { local fp list; fp=$(mok_fp); list=$(mokutil --list-enrolled 2>/dev/null | tr 'A-F' 'a-f'); [ -n "$fp" ] && [[ $list == *"$fp"* ]]; }   # 不走 grep 管道: mokutil 退出码非零 + pipefail 会把匹配成功也判失败(10-17 实撞)

cmd_status() {
    echo "Secure Boot: $(mokutil --sb-state 2>&1)"
    echo "lockdown: $(cat /sys/kernel/security/lockdown)"
    echo "MOK 钥匙: $(ls "$MOK/MOK.der" 2>/dev/null || echo 没造)"
    [ -f "$MOK/MOK.der" ] && { mok_enrolled && echo "MOK 注册: 已注册(指纹 $(mok_fp) 在 MokListRT 里)" || echo "MOK 注册: 没注册"; }
    echo "待注册: $(sudo -n mokutil --list-new 2>/dev/null | grep -c Subject) 把"
    echo "/dev/ashmem: $(ls -l /dev/ashmem 2>&1)"
    echo "模块: $(lsmod | grep '^ashmem_linux' || echo 没载)"
    echo "ashmem.ko: $(ls -l "$KO" 2>&1)"
    echo "内核: $(uname -r)"
}

cmd_enroll() {
    [ -f "$KO" ] || { echo "没有 $KO, 先 make -C src/sandbox/phone/ashmem(要 linux-headers-$(uname -r))"; exit 3; }
    echo "== ① 造 MOK 钥匙(已有就跳过) + 提交注册 =="
    sudo -v || exit 1
    if [ ! -f "$MOK/MOK.der" ]; then
        if command -v update-secureboot-policy >/dev/null; then sudo update-secureboot-policy --new-key
        else sudo mkdir -p "$MOK" && sudo openssl req -new -x509 -newkey rsa:2048 -nodes -days 36500 -keyout "$MOK/MOK.priv" -outform DER -out "$MOK/MOK.der" -subj '/CN=ds4 Secure Boot Module Signature key/'; fi
    fi
    if mok_enrolled; then echo "这把钥匙已经注册过了, 不用重启, 直接跑: $0 finish"; exit 0; fi
    echo; echo "--- 设一个一次性密码(8~16 位, 重启后蓝屏里要再输一次):"
    sudo mokutil --import "$MOK/MOK.der" || exit 1
    echo; echo "--- 待注册清单(应有一把):"
    sudo mokutil --list-new | grep -E "Subject|Issuer" || { echo "没提交成功, 别重启"; exit 1; }   # MokNew 变量 0600 root, 不加 sudo 读到的是空
    echo
    echo "注册已提交。重启后的蓝屏 'Perform MOK management': Enroll MOK → Continue → Yes → 输刚才的密码 → Reboot"
    echo "★蓝屏只停约 10 s, 没按键就直接进系统, 注册作废, 得重跑 enroll★  进系统后再跑: $0 finish"
    read -r -p "现在重启?(y 回车重启, 其他键自己稍后 sudo reboot): " a
    [ "$a" = y ] && sudo reboot
}

cmd_finish() {
    echo "== ② 核对 MOK =="
    mok_enrolled || { echo "MOK 没注册(蓝屏那步没做成). 重来: $0 enroll"; cmd_status; exit 1; }
    echo "已注册。== 签名 + 装模块 + 开机自动加载(phone_prepare_linux.sh ⑤, 要 sudo 密码) =="
    [ -x "$ROOT/ds4-phone" ] || { echo "先 make ds4-phone"; exit 3; }
    sudo "$ROOT/scripts/phone_prepare_linux.sh" || { echo "准备脚本失败(看上面)"; exit 1; }
    [ -c /dev/ashmem ] || { echo "/dev/ashmem 还是没有"; cmd_status; exit 1; }
    echo "宿主 /dev/ashmem 在了。"
    cmd_test
}

cmd_test() {   # 模块装好之后单独可跑(工作台刚起来时用 test, 不必再过一遍 sudo)
    [ -c /dev/ashmem ] || { echo "宿主没有 /dev/ashmem, 先 finish"; exit 1; }
    echo "== 重启云手机, 再开 12306 看 60 s =="
    TOK=$(cat ~/.ds4/assistant/token 2>/dev/null) || { echo "没有 ~/.ds4/assistant/token: 工作台没初始化过, 模块已装好, 起工作台后再跑一次 finish"; exit 0; }
    H="Authorization: Bearer $TOK"; A='X-DS4-Assistant: 1'; J='Content-Type: application/json'; P="http://127.0.0.1:$PORT/api/phone"
    curl -s --noproxy '*' -m 5 -H "$H" "$P/status" | grep -q supported || { echo "工作台没在跑(127.0.0.1:$PORT 不应答): 模块已装好, 先起工作台再跑一次 finish"; exit 0; }
    curl -s --noproxy '*' -m 60 -X POST -H "$H" -H "$A" "$P/disable" >/dev/null; sleep 2
    curl -s --noproxy '*' -m 90 -X POST -H "$H" -H "$A" "$P/enable" >/dev/null
    for _ in $(seq 1 40); do curl -s --noproxy '*' -m 10 -H "$H" "$P/status" | grep -q '"state":"ready"' && break; sleep 3; done
    curl -s --noproxy '*' -m 10 -H "$H" "$P/status" | grep -q '"state":"ready"' || { echo "手机没 ready:"; curl -s --noproxy '*' -m 10 -H "$H" "$P/status"; echo; exit 1; }
    ADB="$ROOT/tests/phone/adbprobe"
    for _ in $(seq 1 30); do [ "$("$ADB" shell 'getprop sys.boot_completed' | head -1)" = 1 ] && break; sleep 3; done
    echo "手机里 /dev/ashmem: $("$ADB" shell 'ls -l /dev/ashmem' | head -1)"
    "$ADB" shell 'logcat -b events -c' >/dev/null
    curl -s --noproxy '*' -m 60 -X POST -H "$H" -H "$A" -H "$J" -d '{"package":"com.MobileTicket"}' "$P/open" >/dev/null; sleep 8
    # 首次运行(或 pm clear 后)的三道门: 系统"全屏提示"GOT IT → 隐私弹窗 AGREE → 三页引导 → "即刻体验"; 已经进过首页的直接到 MainActivity, 下面的点击落空无害
    if "$ADB" shell 'dumpsys window | grep mFocusedApp' | grep -q WelcomeGuide; then
        "$ADB" shell 'input tap 552 401; sleep 1; input tap 510 945; sleep 5; input swipe 600 640 120 640 300; sleep 1; input swipe 600 640 120 640 300; sleep 1; input swipe 600 640 120 640 300; sleep 1.5; input tap 360 1138' >/dev/null
    fi
    echo "等 60 s …"; sleep 60
    echo "焦点: $("$ADB" shell 'dumpsys window | grep mFocusedApp' | head -1)"
    echo "主进程死亡次数(60 s): $("$ADB" shell 'logcat -b events -d | grep -c "am_proc_died.*com.MobileTicket,"' | head -1)"
    curl -s --noproxy '*' -m 20 -H "$H" -o "$ROOT/gguf/phone/12306_after_ashmem.png" "$P/screen" && echo "截图: $ROOT/gguf/phone/12306_after_ashmem.png(工作台云手机页签也能直接看)"
    echo "判定: 焦点在 com.MobileTicket/.ui.activity.MainActivity 且死亡次数 0 = 通; 否则把上面几行原样发我"
}

case "${1:-}" in
    status) cmd_status ;;
    enroll) cmd_enroll ;;
    finish) cmd_finish ;;
    test)   cmd_test ;;
    *) echo "用法: $0 status|enroll|finish|test(说明见文件头)"; exit 2 ;;
esac
