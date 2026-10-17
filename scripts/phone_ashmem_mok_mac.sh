#!/bin/bash
# phone_ashmem_mok_mac.sh — 在 Mac 上经 ssh 给 spark 装 ashmem 内核模块(云手机里老 WebView 内核的 app, 如 12306/饿了么, 没有 /dev/ashmem 就自杀)。
# 10-17。背景见 docs/assistant_phone.md §11.2 与 fable5 "云手机装 12306"。
#
# 怎么用(分两段, 中间必须你到 spark 机器跟前按一次键 —— Secure Boot 下这一步没有远程办法):
#   scripts/phone_ashmem_mok_mac.sh status   # 看现状: Secure Boot / MOK 注册没 / /dev/ashmem 在不在
#   scripts/phone_ashmem_mok_mac.sh enroll   # 第一段: 造 MOK 钥匙 → 提交注册(让你设一个一次性密码, 8~16 位, 待会蓝屏里要输) → 重启 spark
#       ↓ 重启时接显示器+键盘, 蓝屏 "Perform MOK management": Enroll MOK → Continue → Yes → 输那个密码 → Reboot
#   scripts/phone_ashmem_mok_mac.sh finish   # 第二段: 等 spark 回来 → 核对已注册 → 签名+装模块(仓内 scripts/phone_prepare_linux.sh ⑤) → 重启云手机 → 开 12306 看 60 s 死不死
#
# 会提示输密码的地方: enroll 里 sudo 一次、mokutil 设一次性密码两次(输入时不回显); finish 里 sudo 一次。都是 spark 上的终端交互, 所以全程 ssh -t。
# 出错会怎样: enroll 结尾 `mokutil --list-new` 没列出钥匙 = 注册没提交, 别重启, 看上面的报错重跑;
#            finish 报 "MOK 没注册" = 蓝屏那步没做成(常见: 开机没按键, 超时直接进系统了), 再 enroll 一次重来;
#            finish 报 "Key was rejected" = 签名用的钥匙不是注册的那把, 看 /var/lib/shim-signed/mok/ 是不是被换过。
set -uo pipefail
HOST=spark                                  # ~/.ssh/config 里的别名(直连主机名会 denied, 见 memory)
REPO='~/ds4-main'                           # spark 上的仓库(单引号: 在远端展开)
MOK=/var/lib/shim-signed/mok                # Ubuntu DKMS 用的那把 MOK, phone_prepare_linux.sh ⑤ 签名也用它
HERE="$(cd "$(dirname "$0")/.." && pwd)"

fp() { r "openssl x509 -inform DER -in $MOK/MOK.der -noout -fingerprint -sha1 2>/dev/null | cut -d= -f2 | tr A-F a-f"; }   # 注册与否按指纹查 MokListRT; `--test-key` 在 root 下会误答 not enrolled(10-17 实撞)
r()  { ssh "$HOST" "$@"; }                  # 不要终端
rt() { ssh -t "$HOST" "$@"; }               # 要终端(sudo / mokutil 的密码提示)

cmd_status() {
    r "echo \"Secure Boot: \$(mokutil --sb-state 2>&1)\"; echo \"lockdown: \$(cat /sys/kernel/security/lockdown)\";
       echo \"MOK 钥匙: \$(ls $MOK/MOK.der 2>/dev/null || echo 没造)\";
       if [ -f $MOK/MOK.der ]; then fp=\$(openssl x509 -inform DER -in $MOK/MOK.der -noout -fingerprint -sha1 | cut -d= -f2 | tr A-F a-f); mokutil --list-enrolled | tr A-F a-f | grep -q \"\$fp\" && echo \"MOK 注册: 已注册(\$fp)\" || echo \"MOK 注册: 没注册\"; fi;
       echo \"待注册: \$(sudo -n mokutil --list-new 2>&1 | grep -c Subject) 把\";
       echo \"/dev/ashmem: \$(ls -l /dev/ashmem 2>&1)\"; echo \"模块: \$(lsmod | grep ^ashmem_linux || echo 没载)\";
       echo \"ashmem.ko: \$(ls -l $REPO/src/sandbox/phone/ashmem/ashmem_linux.ko 2>&1)\"; echo \"内核: \$(uname -r)\""
}

cmd_enroll() {
    echo "== 第一段: 在 spark 上造 MOK 钥匙并提交注册(下面会要 sudo 密码, 然后 mokutil 让你设一次性密码两遍) =="
    rt "set -e; sudo -v;
        if [ ! -f $MOK/MOK.der ]; then
            if command -v update-secureboot-policy >/dev/null; then sudo update-secureboot-policy --new-key;
            else sudo mkdir -p $MOK && sudo openssl req -new -x509 -newkey rsa:2048 -nodes -days 36500 -keyout $MOK/MOK.priv -outform DER -out $MOK/MOK.der -subj '/CN=ds4 Secure Boot Module Signature key/'; fi;
        fi;
        echo; echo '--- 设一个一次性密码(8~16 位, 蓝屏里要再输一次):'; sudo mokutil --import $MOK/MOK.der;
        echo; echo '--- 待注册清单(应有一把):'; sudo mokutil --list-new | grep -E 'Subject|Issuer' || { echo '没提交成功, 别重启'; exit 1; }"
    [ $? -eq 0 ] || exit 1
    echo
    echo "注册已提交。接下来: ①给 spark 接显示器+键盘 ②本脚本重启它 ③开机蓝屏 'Perform MOK management' → Enroll MOK → Continue → Yes → 输刚才的密码 → Reboot"
    echo "★蓝屏停留约 10 s 没按键就直接进系统, 注册作废, 得重来一遍 enroll★"
    read -r -p "显示器键盘接好了就回车重启 spark(Ctrl-C 取消): " _
    rt "sudo reboot" || true
    echo "已发重启。做完蓝屏那步、机器回来后跑: $0 finish"
}

cmd_finish() {
    echo "== 第二段: 等 spark 回来 =="
    for i in $(seq 1 120); do r -o ConnectTimeout=5 true 2>/dev/null && break; sleep 5; [ "$i" = 120 ] && { echo "10 分钟没连上 spark"; exit 1; }; done
    echo "spark 在线, 核对 MOK:"
    list=$(r "mokutil --list-enrolled 2>/dev/null | tr A-F a-f"); [[ $list == *"$(fp)"* ]] || { echo "MOK 没注册(蓝屏那步没做成). 重来: $0 enroll"; cmd_status; exit 1; }
    echo "== 签名 + 装模块(phone_prepare_linux.sh ⑤, 要 sudo 密码) =="
    rt "cd $REPO && ls src/sandbox/phone/ashmem/ashmem_linux.ko >/dev/null && [ -x ds4-phone ] && sudo scripts/phone_prepare_linux.sh" || { echo "准备脚本失败"; exit 1; }
    r "[ -c /dev/ashmem ]" || { echo "/dev/ashmem 还是没有"; cmd_status; exit 1; }
    echo "== 重启云手机(主进程接口), 再开 12306 看 60 s =="
    r "cd $REPO && TOK=\$(cat ~/.ds4/assistant/token); H=\"Authorization: Bearer \$TOK\"; A='X-DS4-Assistant: 1'; J='Content-Type: application/json'; P=http://127.0.0.1:8000/api/phone;
       curl -s --noproxy '*' -m 60 -X POST -H \"\$H\" -H \"\$A\" \$P/disable >/dev/null; sleep 2;
       curl -s --noproxy '*' -m 90 -X POST -H \"\$H\" -H \"\$A\" \$P/enable >/dev/null;
       for i in \$(seq 1 40); do curl -s --noproxy '*' -m 10 -H \"\$H\" \$P/status | grep -q '\"state\":\"ready\"' && break; sleep 3; done;
       for i in \$(seq 1 30); do [ \"\$(./tests/phone/adbprobe shell 'getprop sys.boot_completed' | head -1)\" = 1 ] && break; sleep 3; done;
       echo \"手机里 /dev/ashmem: \$(./tests/phone/adbprobe shell 'ls -l /dev/ashmem' | head -1)\";
       ./tests/phone/adbprobe shell 'logcat -b events -c' >/dev/null;
       curl -s --noproxy '*' -m 60 -X POST -H \"\$H\" -H \"\$A\" -H \"\$J\" -d '{\"package\":\"com.MobileTicket\"}' \$P/open >/dev/null; sleep 8;
       ./tests/phone/adbprobe shell 'dumpsys window | grep mFocusedApp | grep -q WelcomeGuide && input tap 360 1138' >/dev/null; sleep 60;
       echo \"焦点: \$(./tests/phone/adbprobe shell 'dumpsys window | grep mFocusedApp' | head -1)\";
       echo \"主进程死亡次数(60 s): \$(./tests/phone/adbprobe shell 'logcat -b events -d | grep -c \"am_proc_died.*com.MobileTicket,\"' | head -1)\";
       curl -s --noproxy '*' -m 20 -H \"\$H\" -o /tmp/12306_after_ashmem.png \$P/screen"
    scp -q "$HOST:/tmp/12306_after_ashmem.png" "$HERE/gguf/phone/12306_after_ashmem.png" && echo "截图: $HERE/gguf/phone/12306_after_ashmem.png" && open "$HERE/gguf/phone/12306_after_ashmem.png" 2>/dev/null
    echo "判定: 焦点在 com.MobileTicket/.ui.activity.MainActivity 且死亡次数 0 = 通; 否则把上面几行原样发我"
}

case "${1:-}" in
    status) cmd_status ;;
    enroll) cmd_enroll ;;
    finish) cmd_finish ;;
    *) echo "用法: $0 status|enroll|finish(说明见文件头)"; exit 2 ;;
esac
