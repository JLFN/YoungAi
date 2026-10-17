#!/bin/bash
# posttrain_svg_compare.sh — 同一句画图请求, ③ 卸着 / 挂着各问一次, 把回答里的第一个 ```html / ```svg 代码块落成文件, 给浏览器并排看(2026-10-17 鹈鹕后训练验收)。
#   用法: posttrain_svg_compare.sh <③ 目录(ckpt_eNN)> <输出目录> "问句" ["问句" ...]
#   走主进程 :8000 的 /v1/chat/completions(经转发到模型子进程, 采样默认配方, 不传温度/长度 —— 与聊天页同一条路), 开关 ③ 走 POST /api/models/plugins
#   (与聊天页"后训练"开关同一条路, 侧车保持当前 serve_pick 里那套)。回答原文全部落 <输出目录>/<编号>_<arm>.txt, 代码块落 .html / .svg; 结尾把 ③ 卸掉。
#   口令读 ~/.ds4/assistant/token(10-12 起主进程有登录门)。
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"; cd "$ROOT" || exit 1
PT="${1:?③ 目录}"; OUT="${2:?输出目录}"; shift 2
[ $# -ge 1 ] || { echo "要至少一句问句"; exit 1; }
mkdir -p "$OUT"
TOK=$(cat ~/.ds4/assistant/token 2>/dev/null || true)
U="http://127.0.0.1:8000"
C() { curl -s --noproxy '*' --max-time 1800 ${TOK:+-H "Authorization: Bearer $TOK"} "$@"; }
ZC=$(C "$U/api/models/list" | jq -r '.loaded.zchain // "none"')
say() { echo "[$(date +%H:%M:%S)] $*"; }
ask_all() {   # $1 = arm 名
    local arm="$1" i=0 q body r txt
    for q in "${QS[@]}"; do
        i=$((i + 1))
        body=$(jq -cn --arg q "$q" '{model:"ds4",messages:[{role:"user",content:$q}],stream:false}')
        r=$(C -H 'Content-Type: application/json' -d "$body" "$U/v1/chat/completions")
        txt=$(printf '%s' "$r" | jq -r '.choices[0].message.content // (.error|tostring) // .')
        printf '%s' "$txt" > "$OUT/${i}_${arm}.txt"
        # 第一个围栏代码块: ```html 或 ```svg(没有围栏就把整段当 html)
        awk -v o="$OUT/${i}_${arm}" 'BEGIN{f=0} /^```(html|svg)/{if(!f){f=1;ext=($0~/svg/)?"svg":"html";next}} /^```/{if(f==1){f=2}} f==1{print > (o"."ext)}' "$OUT/${i}_${arm}.txt"
        [ -f "$OUT/${i}_${arm}.html" ] || [ -f "$OUT/${i}_${arm}.svg" ] || cp "$OUT/${i}_${arm}.txt" "$OUT/${i}_${arm}.html"
        say "$arm #$i: 回答 $(printf '%s' "$txt" | wc -c) 字节, 用量 $(printf '%s' "$r" | jq -c '.usage // empty'), finish $(printf '%s' "$r" | jq -r '.choices[0].finish_reason // "?"')"
    done
}
QS=("$@")
say "卸 ③: $(C -H 'Content-Type: application/json' -d "{\"zchain\":\"$ZC\",\"posttrain\":\"\"}" "$U/api/models/plugins")"; sleep 6
ask_all base
say "挂 ③ $PT: $(C -H 'Content-Type: application/json' -d "{\"zchain\":\"$ZC\",\"posttrain\":\"$PT\"}" "$U/api/models/plugins")"; sleep 6
say "装着: $(C "$U/api/models/list" | jq -c .loaded)"
ask_all pt
say "卸 ③: $(C -H 'Content-Type: application/json' -d "{\"zchain\":\"$ZC\",\"posttrain\":\"\"}" "$U/api/models/plugins")"; sleep 6
say "完; 产物 $OUT"; ls -la "$OUT"
