#!/bin/bash
# vision_box_probe.sh — 量现役模型"看图定位"准不准: 把一张图 + 一个问题直接发给模型子进程(ds4-server /v1/chat/completions, 不经助理、无工具、无记忆),
#   打印回答原文与思考长度。10-15 用途: 改图工具(docs/assistant_image.md)要不要让模型报包围框 —— 框准就走"框 + 颜色模型", 框飘就退回"点 + 泛洪"。
# 用法: tests/asst/vision_box_probe.sh 图片路径 "问题" [端口=8001] [温度=0]
#   图走 OpenAI image_url(data: URL), 和助理 asst_image.c 同一条路; 温度默认 0(尺必须显式温 0, 2026-09-28 铁律)。
set -uo pipefail
IMG="${1:?图片路径}"; Q="${2:?问题}"; PORT="${3:-8001}"; TEMP="${4:-0}"
[ -f "$IMG" ] || { echo "没有 $IMG"; exit 1; }
BODY=$(mktemp)
python3 - "$IMG" "$Q" "$TEMP" > "$BODY" <<'EOF'
import json, sys, base64, os
img, q, temp = sys.argv[1], sys.argv[2], float(sys.argv[3])
ext = os.path.splitext(img)[1].lower().lstrip('.'); mime = 'image/jpeg' if ext in ('jpg', 'jpeg') else 'image/' + ext
b64 = base64.b64encode(open(img, 'rb').read()).decode()
print(json.dumps({"model": "deepseek-v4-flash", "temperature": temp, "max_tokens": 4096,
                  "messages": [{"role": "user", "content": [{"type": "text", "text": q}, {"type": "image_url", "image_url": {"url": f"data:{mime};base64,{b64}"}}]}]}))
EOF
t0=$(date +%s.%N)
R=$(curl -s --noproxy '*' -m 900 -H 'Content-Type: application/json' -d @"$BODY" "http://127.0.0.1:$PORT/v1/chat/completions")
t1=$(date +%s.%N)
rm -f "$BODY"
python3 - "$R" "$t0" "$t1" <<'EOF'
import json, sys
r = json.loads(sys.argv[1]); dt = float(sys.argv[3]) - float(sys.argv[2])
if 'error' in r: print('error:', r['error']); sys.exit(1)
m = r['choices'][0]['message']; u = r.get('usage', {})
rc = m.get('reasoning_content') or m.get('reasoning') or ''
print(f"用时 {dt:.1f} s; prompt {u.get('prompt_tokens')} / completion {u.get('completion_tokens')} tokens; 思考 {len(rc)} 字符")
print('== 回答原文'); print(m.get('content'))
EOF
