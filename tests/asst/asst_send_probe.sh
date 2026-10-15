#!/bin/bash
# asst_send_probe.sh — 不开页面, 直接从本机把一条消息发给正在跑的工作台助理(ds4-train), 等某个工具调用结束, 打印它的结果。
# 用途: 复现"只有在生产 ds4-train 进程里才出"的工具问题(10-13 实撞: make_video 的沙盒子进程段错误, 单测二进制里同样输入全正常),
#   以及事后看模型这一轮调了什么。走的就是页面那条路(POST /api/assistant/send), 所以模型、提示词、沙盒、子进程全是生产的。
# 用法: tests/asst/asst_send_probe.sh "消息" [等的工具名=make_video] [超时秒=600] [端口=8000] [会话 id(空 = 新会话)] [附件(工作区相对路径, 逗号分隔)]
#   附件写 workspace/ 下已有的路径(页面上传后落 inbox/<uuid>.jpeg, 第 6 个参数就填 "inbox/<uuid>.jpeg"), 和页面点附件按钮发出的 files 字段同一条路,
#   图片会被 asst_image.c 展成 image_url 块给模型看(10-15 复现"衣服变黑"思考段死循环用的就是这条)。
#   接着上一条聊(10-15 计划模式真模型门: 提计划 → 同一段对话里回答问题)就把上一次打印的 session 填进第 5 个参数; 只等这条消息之后的 end。
#   最后打印这条消息之后模型的回答原文(给人看原样输出, 判读只作参考)。
#   消息写 --start(要带第 5 个参数): 不发消息, 替这段对话的计划按"开始"(POST tasks {op:start}, 和页面计划卡同一条路), 之后照样等这一轮跑完。
#   口令读 ~/.ds4/assistant/token(本机也要带, 不然 studio 登录门会拒); 事件从 ~/.ds4/assistant/sessions/<新会话>/events.jsonl 读(页面的唯一真相)。
set -uo pipefail
MSG="${1:?消息}"; TOOL="${2:-make_video}"; TMO="${3:-600}"; PORT="${4:-8000}"; SID_IN="${5:-}"; FILES="${6:-}"
TOK=$(cat ~/.ds4/assistant/token) || { echo "没有 ~/.ds4/assistant/token"; exit 1; }
BODY=$(python3 -c 'import json,sys; print(json.dumps({"session":sys.argv[2],"text":sys.argv[1],"client":"probe","device":{"kind":"desktop","tz":"Asia/Shanghai","lang":"zh-CN"},"caps":["file"],"apps":[],"files":[f for f in sys.argv[3].split(",") if f]}))' "$MSG" "$SID_IN" "$FILES")
EV0=0; [ -n "$SID_IN" ] && EV0=$(wc -l < "$HOME/.ds4/assistant/sessions/$SID_IN/events.jsonl" 2>/dev/null || echo 0)   # 接着聊: 之前的事件不算
if [ "$MSG" = "--start" ]; then
    [ -n "$SID_IN" ] || { echo "--start 要第 5 个参数(会话 id)"; exit 1; }
    TID=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("task",""))' "$HOME/.ds4/assistant/sessions/$SID_IN/meta.json")
    [ -n "$TID" ] || { echo "会话 $SID_IN 没有计划"; exit 1; }
    R=$(curl -s --noproxy '*' -m 10 -H "X-DS4-Assistant: 1" -H "Authorization: Bearer $TOK" -H 'Content-Type: application/json' -d "{\"op\":\"start\",\"id\":\"$TID\"}" "http://127.0.0.1:$PORT/api/assistant/tasks")
    printf '%s' "$R" | grep -q '"error"' && { echo "start 失败: $R"; exit 1; }
    SID="$SID_IN"
else
    R=$(curl -s --noproxy '*' -m 10 -H "X-DS4-Assistant: 1" -H "Authorization: Bearer $TOK" -H 'Content-Type: application/json' -d "$BODY" "http://127.0.0.1:$PORT/api/assistant/send")
    SID=$(printf '%s' "$R" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("session",""))' 2>/dev/null)
    [ -n "$SID" ] || { echo "send 失败: $R"; exit 1; }
fi
EV="$HOME/.ds4/assistant/sessions/$SID/events.jsonl"
echo "session $SID"
t0=$(date +%s)
while :; do
    if [ -s "$EV" ] && tail -n +$((EV0 + 1)) "$EV" | grep -q "\"ev\":\"end\""; then break; fi
    [ $(( $(date +%s) - t0 )) -ge "$TMO" ] && { echo "★${TMO}s 内没结束★"; break; }
    sleep 5
done
echo "用时 $(( $(date +%s) - t0 )) s"
echo "== 工具调用"; tail -n +$((EV0 + 1)) "$EV" | grep '"ev":"tool"' | cut -c1-300
echo "== $TOOL 结果"; python3 - "$EV" "$TOOL" "$EV0" <<'EOF'
import json, sys
ids, text = set(), []
for l in open(sys.argv[1], "rb").read().decode("utf-8", "replace").splitlines()[int(sys.argv[3]):]:
    e = json.loads(l)
    if e.get("ev") == "tool" and e["name"] == sys.argv[2]: ids.add(e["id"])
    if e.get("ev") == "tool_end" and e["id"] in ids: print("ok" if e["ok"] else "FAIL", e["ms"], "ms\n", e["result"][:3000])
    if e.get("ev") == "tool_end" and e.get("decision") == "blocked": print("blocked:", e["id"], e["result"][:200])
    if e.get("ev") == "text": text.append(e["d"])
    if e.get("ev") == "end": print("end:", e["reason"])
print("== 回答原文\n" + "".join(text))
EOF
