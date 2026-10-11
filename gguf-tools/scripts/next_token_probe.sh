#!/bin/bash
# next_token_probe.sh — 在一条真实请求的某个位置重复采样下一个 token, 数各 token 的占比(2026-10-12)。
#   用法: next_token_probe.sh <trace 文件> <请求号> [采样次数 16] [模型端口 8001]
#   取 trace 里第 <请求号> 条请求的"rendered prompt" + 它的"generated text"(模型吐到收口前的全文)拼成提示, 以 raw:true 发
#   /v1/completions, 每次只要 1 个 token, 温度照模型卡(不传 temperature), 发 N 次; 从 trace 的 token ids 块读每次采到的 id 计数。
#   为什么能便宜: N 次提示完全相同, 第 2 次起走同提示 KV 复用(core_v41_kvreuse.c), 一块都不预填, 一次就是一步解码。
#   用途: 10-12 实撞 —— 助理第 12 步说完"我再确认一下…"直接采到 EOS(id 1)收口, 要知道这一位 EOS 的概率是 2% 还是 30%,
#   服务端没有 logprobs 输出, 只能重复采样数。
#   前提核: 第一次请求的提示 token 数应 = 该请求 prompt_tokens + generated_tokens(拼接后重新分词没粘连); 不等脚本直接说。
#   出错会怎样: 请求号不存在 → "找不到请求"; 第 2 次起 trace 里没有 "kv reuse: n of n" → 同提示复用没生效, 打 WARN 但照数。
set -uo pipefail
TRACE="${1:?trace 文件}"; RQ="${2:?请求号}"; N="${3:-16}"; PORT="${4:-8001}"
U="http://127.0.0.1:$PORT/v1/completions"
TR=$(cat gguf/v41/night/trace/current 2>/dev/null) || { echo "找不到 gguf/v41/night/trace/current(在仓库根目录跑, 服务要开 --trace)"; exit 1; }
S=$(grep -n "^===== request $RQ " "$TRACE" | head -1 | cut -d: -f1); [ -n "$S" ] || { echo "找不到请求 $RQ"; exit 1; }
E=$(awk -v s="$S" 'NR>s && /^===== request /{print NR; exit}' "$TRACE"); [ -n "$E" ] || E=$(wc -l < "$TRACE")
# rendered prompt 段 = "--- rendered prompt ---" 下一行起到 "--- generated text ---" 前一行; trace 写段时前后各垫一个空行, 去掉
seg() { sed -n "${S},${E}p" "$TRACE" | awk -v a="$1" -v b="$2" '$0==a{p=1; next} $0==b{exit} p' | sed '1{/^$/d}' | sed -e :x -e '/^\n*$/{$d;N;bx' -e '}'; }
PROMPT=$(seg "--- rendered prompt ---" "--- generated text ---"; printf x); PROMPT="${PROMPT%x}"
GEN=$(sed -n "${S},${E}p" "$TRACE" | awk '/^--- generated text ---/{p=1; next} /^--- parsed message ---/{exit} p && !/^--- trace:/' | sed -e :x -e '/^\n*$/{$d;N;bx' -e '}'; printf x); GEN="${GEN%x}"
GEN="${GEN#"${GEN%%[![:space:]]*}"}"   # 段首的空行是 trace 垫的
while [ "${GEN%$'\n'}" != "$GEN" ]; do GEN="${GEN%$'\n'}"; done   # 段尾的换行是 trace 的行终止符, 不是模型吐的(10-12 实撞: 多了它采样位就错到"换行之后", 14/16 开工具块)
PT=$(sed -n "${S},${E}p" "$TRACE" | grep -m1 "^--- token ids: prompt" | sed 's/.*prompt \([0-9]*\), generated \([0-9]*\).*/\1 \2/')
NP=$(echo "$PT" | cut -d' ' -f1); NG=$(echo "$PT" | cut -d' ' -f2)
# 生成 id 里最后一个是 EOS(id 1)的话, 文本里没有它: 拼接后的提示应比 prompt+generated 少 1
LASTID=$(sed -n "${S},${E}p" "$TRACE" | awk '/^--- token ids:/{h=2; next} h==2{h=1; next} h==1{print; h=0}' | head -1 | sed 's/^generated: *//' | awk '{print $NF}')
[ "$LASTID" = "1" ] && NG=$((NG - 1))
echo "请求 $RQ: 提示 $NP token + 已生成 $NG token(原请求收口于 id $LASTID); 采样 $N 次, 每次 1 个"
echo "续写点前 120 字: $(printf '%s' "$GEN" | tail -c 120 | tr '\n' ' ')"
# 服务端先回 HTTP、收尾才写 token ids 块(头 + prompt 行 + generated 行): 等头的数涨了, 再等 generated 行落盘
nblk() { grep -c '^--- token ids:' "$TR"; }
wait_blk() {
    for _ in $(seq 1 3000); do [ "$(nblk)" -gt "$1" ] && break; sleep 0.1; done
    [ "$(nblk)" -gt "$1" ] || { echo "等了 300 s trace 里没有新的 token ids 块"; exit 1; }
    for _ in $(seq 1 50); do [ -n "$(last_gen_id)" ] && [ "$(awk '/^--- token ids:/{c++} END{print c+0}' "$TR")" -eq "$(grep -c '^generated: ' "$TR")" ] && return 0; sleep 0.1; done
}
# 提示有几十万字节: 走 --arg 会撞 ARG_MAX("参数列表过长"), 落文件让 jq -Rs 读
PF=/tmp/next_token_probe_prompt.txt
req() { printf '%s' "$1" > "$PF"; jq -Rs '{prompt:., raw:true, max_tokens:1}' "$PF" | curl -s --noproxy '*' --max-time 900 "$U" -H 'Content-Type: application/json' -d @-; }
last_gen_id() { awk '/^--- token ids:/{h=2; next} h==2{h=1; next} h==1{print; h=0}' "$TR" | tail -1 | sed 's/^generated: *//' | cut -d' ' -f1; }
last_ptok() { grep '^--- token ids: prompt' "$TR" | tail -1 | sed 's/.*prompt \([0-9]*\),.*/\1/'; }
declare -A CNT; declare -A TXT
for ((i = 1; i <= N; i++)); do
    n0=$(nblk); out=$(req "$PROMPT$GEN"); wait_blk "$n0"
    id=$(last_gen_id); t=$(echo "$out" | jq -r '.choices[0].text' | head -c 40 | tr '\n' '⏎')
    if [ "$i" -eq 1 ]; then
        want=$((NP + NG)); got=$(last_ptok)
        [ "$got" -eq "$want" ] || echo "WARN 前提: 拼接后提示 $got token ≠ 原请求 $want(重新分词粘连了, 位置可能差一两个 token)"
    else
        grep -o 'kv reuse: [0-9]* of [0-9]*' "$TR" | tail -1 | grep -q "kv reuse: $(last_ptok) of $(last_ptok)" || echo "WARN 第 $i 次没走同提示复用"
    fi
    CNT[$id]=$(( ${CNT[$id]:-0} + 1 )); TXT[$id]="$t"
    echo "  第 $i 次: id $id  $(printf %q "$t")"
done
echo "== 占比(id  次数  文本):"
for id in "${!CNT[@]}"; do printf "%8s %4d/%d  %s\n" "$id" "${CNT[$id]}" "$N" "$(printf %q "${TXT[$id]}")"; done | sort -k2 -nr
