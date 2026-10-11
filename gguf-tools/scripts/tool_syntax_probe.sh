#!/bin/bash
# tool_syntax_probe.sh — 同一个请求打 N 次(非流式, 默认采样), 数: 成功出 tool_calls 几次、正文里漏出裸 <parameter 几次、
#   正文开头是英文几次、非思考模式下冒出 </think>(服务端切进 reasoning_content)几次(2026-10-11)。
#   用法: tool_syntax_probe.sh <请求体 json> [次数 5] [模型端口 8001]
#   背景: V4.1 的参数开标签应为 "<｜DSML｜ parameter", 模型有时写成裸 "<parameter"(闭标签却是 DSML 的), 解析失败整段退回正文、finish=stop。
#   每次原样记: finish_reason、tool_calls 个数、正文里以 <parameter 开头的行数; 日志 /tmp/tool_syntax_<MMDDHHMM>.log。
set -uo pipefail
BODY="${1:?请求体 json}"; N="${2:-5}"; PORT="${3:-8001}"
LOG="/tmp/tool_syntax_$(date +%m%d%H%M).log"
ok=0; bare=0; en=0; th=0
for i in $(seq 1 "$N"); do
    r=$(curl -s --noproxy '*' --max-time 180 -H 'Content-Type: application/json' --data-binary @"$BODY" "http://127.0.0.1:$PORT/v1/chat/completions")
    f=$(printf '%s' "$r" | jq -r '.choices[0].finish_reason // "ERR"')
    nc=$(printf '%s' "$r" | jq -r '[.choices[0].message.tool_calls[]?] | length')
    nb=$(printf '%s' "$r" | jq -r '.choices[0].message.content // ""' | grep -c '^<parameter')
    c1=$(printf '%s' "$r" | jq -r '.choices[0].message.content // ""' | head -c 1)
    rz=$(printf '%s' "$r" | jq -r '.choices[0].message.reasoning_content // "" | length' 2>/dev/null); rz=${rz:-0}   # 请求失败时 jq 没输出: 当 0, 别让下一行的整数比较报错
    case "$c1" in [A-Za-z]) en=$((en + 1));; esac; [ "$rz" -gt 0 ] && th=$((th + 1))
    [ "$f" = "tool_calls" ] && ok=$((ok + 1)); [ "$nb" -gt 0 ] && bare=$((bare + 1))
    printf '#%d finish=%s calls=%s 裸parameter行=%s 首字=%s 思考段字数=%s\n' "$i" "$f" "$nc" "$nb" "$c1" "$rz" | tee -a "$LOG"
    printf '%s\n' "$r" >> "$LOG"
done
echo "$(basename "$BODY"): tool_calls 成功 $ok/$N, 漏出裸 <parameter $bare/$N, 英文开头 $en/$N, 冒出 </think> $th/$N (原文 $LOG)"
