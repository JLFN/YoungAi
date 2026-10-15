#!/bin/bash
# oneapp_probe.sh — 真机门: "一次只开一个 app"(docs/assistant_phone.md §11 关 3, 10-15 用户定)。在 spark 上跑, 经真实 REST(主进程端口 + 本机口令)。
#   ① open A → open B: A 的进程应全没了(清场在 am start 之前);
#   ② 不经 REST 直接 am start A(= 页面点桌面图标: input tap 立刻回来, 焦点还在旧 app)并马上做一个动作: A 刚起(< 5 s)不许被误杀, B 还在;
#   ③ 6 s 后再做一个动作: 这时 A 是前台、B 是后台老进程 → B 应被停;
#   ④ 回桌面再动一下: 第三方全停(桌面 = 全关)。
#   用法: tests/phone/oneapp_probe.sh [端口 8000] [A 包名] [B 包名]。出错会怎样: 口令文件不在 → 退 1; 手机没开 → REST 回 409 phone_off。
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 1
PORT="${1:-8000}"; A="${2:-com.tencent.mm}"; B="${3:-com.ss.android.ugc.aweme.mobile}"
TOK=$(cat ~/.ds4/assistant/token) || { echo "没有 ~/.ds4/assistant/token"; exit 1; }
api() { curl -s --noproxy '*' -m 40 -H "Authorization: Bearer $TOK" -H 'Content-Type: application/json' -d "$2" "http://127.0.0.1:$PORT/api/phone/$1" | cut -c1-200; echo; }
procs() { ./tests/phone/adbprobe shell "ps -A -o NAME | grep -E '^($A|$B)' | sort | uniq -c; true" | grep -v '^\[' | grep -v '^$' | sed 's/^/   /'; }
act() { echo "   act → $(api key '{"key":"volume_up"}')"; }   # 最无害的动作: 音量键; 回包里带 "stopped <包>"

echo "== ① open $A"; api open "{\"package\":\"$A\"}"; procs
echo "== ① open $B (A 应没了)"; api open "{\"package\":\"$B\"}"; procs
echo "== ② 直接 am start $A(= 桌面图标), 立刻一个动作: A 刚起不许被杀, B 还在"
./tests/phone/adbprobe shell "am start -n \$(cmd package resolve-activity --brief $A | tail -1) >/dev/null 2>&1" >/dev/null; act; procs
echo "== ③ 6 s 后再动一下: B 应被停, A 在"; sleep 6; act; procs
echo "== ④ 回桌面再动一下: 全停"; api key '{"key":"home"}' >/dev/null; sleep 2; act; procs
echo "== 焦点"; ./tests/phone/adbprobe shell "dumpsys window | grep mFocusedApp" | head -1
