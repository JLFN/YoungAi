#!/bin/bash
# plugin_switch_probe.sh — 量"后训练开关"的切换体验(2026-10-17): 切换本身多久、切完下一条消息接不接得上检查点。
#   用法: plugin_switch_probe.sh [提示文件 speed-bench/fin_ctx_prompt.txt] [页面端口 8000]
#   全程只打主进程 ds4-train 的接口(与浏览器同一条路): 聊天 = POST /v1/chat/completions, 开关 = POST /api/models/plugins,
#   状态 = GET /api/models/list 的 switch.pending, 每条请求的 prompt_tokens / reused / first_token_s 从模型子进程 GET /metrics 读。
#   步骤: ① 问一次(建检查点) → ② 卸 ③ 计时 → ③ 再问同一句(期望 reused = prompt_tokens: 切完预热把检查点用新插件重建了)
#         → ④ 挂 ③ 计时 → ⑤ 再问同一句(同上)。每步的服务日志行(切换 / 预热)原样贴出来。
#   出错会怎样: 没有配得上当前侧车的 ③ 时步骤 ④ 会被主进程拒(400), 脚本原样打出来, 不猜。
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"; cd "$ROOT" || exit 1
PF="${1:-speed-bench/fin_ctx_prompt.txt}"; PORT="${2:-8000}"
U="http://127.0.0.1:$PORT"; M="http://127.0.0.1:8001"
TOK=$(cat ~/.ds4/assistant/token 2>/dev/null || true)
C() { curl -s --noproxy '*' --max-time 900 ${TOK:+-H "Authorization: Bearer $TOK"} "$@"; }
say() { echo "[$(date +%H:%M:%S)] $*"; }
LOGF=$(ls -t gguf/v41/posttrain/ui_logs/server_*.log 2>/dev/null | head -1)

ask() {   # 同一句话问一次: 温 0、8 个 token, 看的是预填不是回答
    local body t0 t1
    body=$(jq -cn --rawfile q "$PF" '{model:"ds4",messages:[{role:"user",content:$q}],temperature:0,max_tokens:8,stream:false}')
    t0=$(date +%s.%N)
    C -H 'Content-Type: application/json' -d "$body" "$U/v1/chat/completions" | jq -r '.choices[0].message.content // (.error|tostring)' | head -c 80; echo
    t1=$(date +%s.%N)
    say "请求 $(awk -v a="$t1" -v b="$t0" "BEGIN{printf \"%.1f\", a-b}") s; 服务账: $(curl -s --noproxy '*' "$M/metrics" | jq -c '.requests[0]|{prompt_tokens,reused,first_token_s,duration_s}')"
}
switch_to() {   # $1 = 后训练目录(空 = 卸); 计时到 switch.pending 回 false
    local z p t0 n
    z=$(C "$U/api/models/list?hub=0" | jq -r '.loaded.zchain'); p="$1"
    t0=$(date +%s.%N)
    say "切换 → 后训练 ${p:-(卸)}: $(C -H 'Content-Type: application/json' -d "$(jq -cn --arg z "$z" --arg p "$p" '{zchain:$z,posttrain:$p}')" "$U/api/models/plugins")"
    for n in $(seq 1 600); do
        sleep 0.3
        [ "$(C "$U/api/models/list?hub=0" | jq -r '.switch.pending')" = false ] && break
    done
    say "pending 清除用时 $(awk -v a="$(date +%s.%N)" -v b="$t0" "BEGIN{printf \"%.1f\", a-b}") s(轮询 0.3 s 一次); 装着: $(C "$U/api/models/list?hub=0" | jq -c '.loaded|{zchain,posttrain}')"
    sleep 1
    [ -n "$LOGF" ] && grep -a "切换插件已排队\|插件已切换\|插件切换没成\|切换后预热\|\[反修\] 挂上\|KV 复用\|prefill .* token" "$LOGF" | tail -6 | cut -c1-200
}

say "提示文件 $PF ($(wc -c < "$PF") 字节); 日志 $LOGF"
LST=$(C "$U/api/models/list?hub=0"); say "装着: $(echo "$LST" | jq -c '.loaded|{zchain,posttrain}')"
PT=$(echo "$LST" | jq -r '.loaded.posttrain // ""')
[ -z "$PT" ] && PT=$(echo "$LST" | jq -r --arg z "$(echo "$LST" | jq -r .loaded.zchain)" '.fnv[$z] as $f | [.posttrains[]|select(.fnv==$f)][0].path // ""')
[ -z "$PT" ] && { say "没有配得上当前侧车的 ③, 开关没东西可挂"; exit 1; }
say "① 建检查点"; ask
say "② 卸 ③"; switch_to ""
say "③ 同一句再问(期望 reused = prompt_tokens)"; ask
say "④ 挂 ③ $PT"; switch_to "$PT"
say "⑤ 同一句再问(期望 reused = prompt_tokens)"; ask
