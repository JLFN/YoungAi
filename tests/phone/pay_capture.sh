#!/bin/bash
# pay_capture.sh — 交易审批门 0(docs/assistant_approval.md §8): 把云手机此刻的屏幕现场抄下来, 给 sbx_pay.c 的表和单测夹具当原料。只读, 不点任何东西。
#   抄三样: ① 守门真正要读的那段 dumpsys(焦点窗口/前台 activity/键盘 + 每个窗口的 fl= 行, 和 sbx_pay.c 发的是同一条命令; 只读 displays 段, 原因见 sbx_pay.c win_cmd);
#           ② uiautomator 控件树;  ③ 截图。
#   用法(spark 上): tests/phone/pay_capture.sh <名字> [输出目录 /tmp/paycap]。出错会怎样: 手机没开 → adbprobe 连不上, 三个文件为空。
#   ★抄下来的东西可能带你的联系人名/聊天内容★: 原样只留在输出目录(默认 /tmp), 进仓库的夹具要手工删到只剩按钮/金额/窗口名。
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 1
NAME="${1:?用法: pay_capture.sh <名字> [输出目录]}"; OUT="${2:-/tmp/paycap}"
mkdir -p "$OUT"
sh() { ./tests/phone/adbprobe shell "$1" | grep -v '^\[[0-9]* ms\]$'; }
sh "dumpsys window displays | grep -E 'mCurrentFocus=|mFocusedApp=|ITYPE_IME frame='; dumpsys window windows | grep -E '^  Window #|^      fl='" > "$OUT/$NAME.win"
sh "rm -f /sdcard/ds4cap.xml; uiautomator dump /sdcard/ds4cap.xml >/dev/null 2>&1; cat /sdcard/ds4cap.xml" > "$OUT/$NAME.xml"
TOK=$(cat ~/.ds4/assistant/token)
curl -s --noproxy '*' -m 20 -H "Authorization: Bearer $TOK" "http://127.0.0.1:8000/api/phone/screen" -o "$OUT/$NAME.png"
echo "== $NAME: $(wc -c < "$OUT/$NAME.win") B dumpsys, $(wc -c < "$OUT/$NAME.xml") B xml, $(wc -c < "$OUT/$NAME.png") B png"
grep -E 'mCurrentFocus|mFocusedApp' "$OUT/$NAME.win"
h=$(sed -nE 's/.*mCurrentFocus=Window\{([0-9a-f]+) .*/\1/p' "$OUT/$NAME.win" | head -1)
[ -n "$h" ] && grep -A1 "Window{$h " "$OUT/$NAME.win" | grep 'fl=' | sed 's/^ */focused fl: /'
