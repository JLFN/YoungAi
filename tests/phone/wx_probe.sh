#!/bin/bash
# wx_probe.sh — 微信写死按钮表的真机门(src/assistant/tools/tool_phone_wx.c): 每一步都按"表里给的坐标"去点, 点完看是不是到了该到的页面,
#   所以过了 = 坐标真点得中, 不只是表能打出来。看屏走 ./ds4_asst_test --phone-look(和模型的 phone_screen 同一条路), 不用重启生产 ds4-train。
#   用法(spark, 仓库根目录, 危险模式开着、云手机 ready、微信已登录): tests/phone/wx_probe.sh <会话行号>
#     会话行号 = 首页第几行是一个普通联系人的聊天(第 1 行常是"订阅号"折叠, 不是聊天)。
#   ★只会在那个聊天的输入框里打一个 a 再删掉, 绝不点发送; 转账只点到面板的格子为止, 不进转账页★。
#   出错会怎样: 某步 FAIL 打出那一帧的原文继续往下走, 最后一行汇总; 手机停在半路的话按几次返回即可。
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 1
ROW="${1:?用法: wx_probe.sh <首页会话行号, 一个普通联系人>}"
pass=0; fail=0; F=""
sh()   { ./tests/phone/adbprobe shell "$1" | grep -v '^\[[0-9]* ms\]$'; }
look() { F=$(./ds4_asst_test --phone-look); }
at()   { printf '%s\n' "$F" | grep -m1 "\"$1\"" | sed -E 's/.* at ([0-9]+),([0-9]+).*/\1 \2/'; }   # 表里某个元素的 x y
tap()  { local c; c=$(at "$1"); if [ -z "$c" ]; then echo "  (表里没有 \"$1\")"; return 1; fi; sh "input tap $c" >/dev/null; sleep 1.2; }
check() { if printf '%s\n' "$F" | grep -qE "$2"; then pass=$((pass+1)); echo "PASS $1"; else fail=$((fail+1)); echo "FAIL $1 (要: $2)"; printf '%s\n' "$F" | sed 's/^/    | /'; fi; }
ybar() { at "$1" | cut -d' ' -f2; }

sh "am force-stop com.tencent.mm; am start -W -n com.tencent.mm/.ui.LauncherUI" >/dev/null; sleep 2
look; check "起来在 Chats 页"            '"Chats tab" \(current tab\)'
printf '%s\n' "$F" | tail -1
tap "+ menu";        look; check "右上 + 菜单弹出"       '"New Chat" .* at 553,197'
sh "input keyevent 4" >/dev/null; sleep 0.8
look; tap "Contacts tab";  look; check "切到 Contacts"        '"Contacts tab" \(current tab\)'
tap "Chats tab";     look; check "切回 Chats"           '"Chats tab" \(current tab\)'
tap "Search";        look; check "进搜索页"             '"Search box" EditText'
sh "input keyevent 4; sleep 0.5; input keyevent 4" >/dev/null; sleep 1
look; tap "Chat row $ROW"; look; check "进第 $ROW 行的聊天"   '"Message box" EditText at [0-9]+,1134'
tap "Message box";   look; check "点输入框: 键盘弹起, 输入栏跟着上移" 'keyboard: shown'
y=$(ybar "Message box"); [ -n "$y" ] && [ "$y" -lt 1000 ] && { pass=$((pass+1)); echo "PASS 输入栏 y=$y(不是原位 1134)"; } || { fail=$((fail+1)); echo "FAIL 输入栏 y=$y"; }
sh "input text a" >/dev/null; sleep 1; look; check "打了字: 出现 Send(键盘上沿)" '"Send" Button at 6[0-9][0-9],[0-9]+'
sh "input keyevent 67" >/dev/null; sleep 1; look; check "删掉: Send 消失回到 +" '"More \(\+\)"'
sh "input keyevent 4" >/dev/null; sleep 1
look; tap "More (+)"; look; check "点 +: 面板八格"        '"Transfer" Button at 440,[0-9]+'
tap "Gallery"; fg=$(sh "dumpsys window displays | grep mCurrentFocus=")
if printf '%s' "$fg" | grep -q 'com.tencent.mm/' && ! printf '%s' "$fg" | grep -q 'LauncherUI'; then pass=$((pass+1)); echo "PASS 点 Gallery 格子离开了聊天页: ${fg##*/}"; else fail=$((fail+1)); echo "FAIL Gallery 后前台: $fg"; fi
for _ in 1 2 3 4 5; do look; printf '%s\n' "$F" | grep -q '"Chats tab"' && break; sh "input keyevent 4" >/dev/null; sleep 0.8; done   # 相册 → 面板 → 聊天 → 首页, 层数看页面
check "按返回退回首页"           '"Chats tab"'
echo "== wx_probe: $pass passed, $fail failed"
