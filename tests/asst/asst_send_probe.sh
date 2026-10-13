#!/bin/bash
# asst_send_probe.sh — 不开页面, 直接从本机把一条消息发给正在跑的工作台助理(ds4-train), 等某个工具调用结束, 打印它的结果。
# 用途: 复现"只有在生产 ds4-train 进程里才出"的工具问题(10-13 实撞: make_video 的沙盒子进程段错误, 单测二进制里同样输入全正常),
#   以及事后看模型这一轮调了什么。走的就是页面那条路(POST /api/assistant/send), 所以模型、提示词、沙盒、子进程全是生产的。
# 用法: tests/asst/asst_send_probe.sh "消息" [等的工具名=make_video] [超时秒=600] [端口=8000]
#   口令读 ~/.ds4/assistant/token(本机也要带, 不然 studio 登录门会拒); 事件从 ~/.ds4/assistant/sessions/<新会话>/events.jsonl 读(页面的唯一真相)。
set -uo pipefail
MSG="${1:?消息}"; TOOL="${2:-make_video}"; TMO="${3:-600}"; PORT="${4:-8000}"
TOK=$(cat ~/.ds4/assistant/token) || { echo "没有 ~/.ds4/assistant/token"; exit 1; }
BODY=$(python3 -c 'import json,sys; print(json.dumps({"session":"","text":sys.argv[1],"client":"probe","device":{"kind":"desktop","tz":"Asia/Shanghai","lang":"zh-CN"},"caps":["file"],"apps":[],"files":[]}))' "$MSG")
R=$(curl -s --noproxy '*' -m 10 -H "X-DS4-Assistant: 1" -H "Authorization: Bearer $TOK" -H 'Content-Type: application/json' -d "$BODY" "http://127.0.0.1:$PORT/api/assistant/send")
SID=$(printf '%s' "$R" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("session",""))' 2>/dev/null)
[ -n "$SID" ] || { echo "send 失败: $R"; exit 1; }
EV="$HOME/.ds4/assistant/sessions/$SID/events.jsonl"
echo "session $SID"
t0=$(date +%s)
while :; do
    if [ -s "$EV" ] && grep -q "\"ev\":\"end\"" "$EV"; then break; fi
    [ $(( $(date +%s) - t0 )) -ge "$TMO" ] && { echo "★${TMO}s 内没结束★"; break; }
    sleep 5
done
echo "== 工具调用"; grep '"ev":"tool"' "$EV" | cut -c1-200
echo "== $TOOL 结果"; python3 - "$EV" "$TOOL" <<'EOF'
import json, sys
ids = set()
for l in open(sys.argv[1], "rb").read().decode("utf-8", "replace").splitlines():
    e = json.loads(l)
    if e.get("ev") == "tool" and e["name"] == sys.argv[2]: ids.add(e["id"])
    if e.get("ev") == "tool_end" and e["id"] in ids: print("ok" if e["ok"] else "FAIL", e["ms"], "ms\n", e["result"][:3000])
    if e.get("ev") == "end": print("end:", e["reason"])
EOF
