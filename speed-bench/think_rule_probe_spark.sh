#!/bin/bash
# think_rule_probe_spark.sh — 反事实打分: 同一条真吐的思考序列, 只换提示词, 看 </think> 的概率变不变(2026-10-18)。
#
# 怎么用(spark 上): speed-bench/think_rule_probe_spark.sh <提示数:ids 文件> [<提示数:ids 文件> ...] [--entry 入环位]
#   例: speed-bench/think_rule_probe_spark.sh 8215:gguf/v41/night/think_rule_1018/orig.ids 8163:gguf/v41/night/think_rule_1018/norule.ids --entry 38449
#   每个 ids = 渲染后的提示 id + 真吐的生成 id(每行一个); 各臂生成段必须一字不差, 只有提示不同, 读数才能逐位对。
# 做什么: 停工作台模型子进程(主进程不动) → 每臂一趟 bugmd_gates_spark.sh scoreids(1017 底座 + 现役编程侧车, top-32)
#   → 每臂打 thinkend / minp 两张账 → 退出时按 gguf/serve_pick.txt 把模型装回。
# 判读: 看"最想停的一位 p"与"入环前/后 </think> 的 Σp"两臂差多少。差一个量级以上 = 提示词那一句就是锁死思考的原因;
#   同量级 = 提示词不是原因, 回到权重/轨迹上找。
# 出错会怎样: ds4-server 没停干净会被 bugmd_gates 的实例锁挡住(报 "ds4-server 还在跑"); 装不回就自己去模型页点加载。
set -uo pipefail
ROOT="$HOME/ds4-main"; cd "$ROOT" || exit 1
M(){ echo "[probe $(date '+%H:%M:%S')] $*"; }
MODEL="$ROOT/gguf/v41/DeepSeek-V4.1-Flash-1017-vq8sh14-q4k-mtpnative.gguf"
AMP="$ROOT/gguf/v41/DeepSeek-V4.1-Flash-1017-vq8sh14-q4k-mtpnative-grrb-code_fit_n15360-engine"
TOOLS="$ROOT/speed-bench/bugmd_ids_tools.py"
PORT=8000; ENTRY=0; ARMS=(); WT2=0
# --wt2: 打分臂之外(或单独)再跑一趟 wt2 守门(v41_judge.sh 引擎臂 裸 / +现役编程侧车), 改引擎前向后用(2026-10-18 补 act_quant(o))
while [ $# -gt 0 ]; do case "$1" in --entry) ENTRY="$2"; shift 2;; --wt2) WT2=1; shift;; *) ARMS+=("$1"); shift;; esac; done
[ ${#ARMS[@]} -ge 1 ] || [ "$WT2" = 1 ] || { echo "用法: $0 [<提示数:ids> ...] [--entry 入环位] [--wt2]"; exit 2; }
for a in "${ARMS[@]}"; do [ -s "${a#*:}" ] || { M "★没有 ${a#*:}★"; exit 1; }; done

restore_model() {
    local pg pz tok body resp
    pg=$(sed -n 1p gguf/serve_pick.txt); pz=$(sed -n 2p gguf/serve_pick.txt)
    pg="${pg#"$ROOT"/}"; pz="${pz#"$ROOT"/}"; [ -n "$pz" ] || pz=none
    tok=$(cat ~/.ds4/assistant/token 2>/dev/null || true)
    body=$(printf '{"gguf":"%s","sidecar":"%s"}' "$pg" "$pz")
    resp=$(curl -s --noproxy '*' --max-time 30 ${tok:+-H "Authorization: Bearer $tok"} -H 'Content-Type: application/json' -d "$body" "http://127.0.0.1:$PORT/api/models/load")
    M "装回 $pg + $pz → $resp"
}

# 只在模型空闲时停它: 有人正在聊就不碰(用户的请求比探针优先)
st=$(curl -s --noproxy '*' --max-time 5 127.0.0.1:8001/metrics | tr ',' '\n' | grep -o '"state":"[a-z]*"' | head -1)
[ -z "$st" ] || [ "$st" = '"state":"idle"' ] || { M "★模型子进程 $st, 不是 idle, 不停★"; exit 1; }
if pgrep -x ds4-server >/dev/null; then
    M "停模型子进程"; pkill -x -TERM ds4-server
    for _ in $(seq 1 60); do pgrep -x ds4-server >/dev/null || break; sleep 2; done
    pgrep -x ds4-server >/dev/null && { pkill -x -KILL ds4-server; sleep 5; }
    pgrep -x ds4-server >/dev/null && { M "★ds4-server 杀不掉★"; exit 1; }
    sleep 5
fi
trap 'rc=$?; restore_model; echo "PROBE_EXIT $rc"' EXIT
M "available $(awk '/MemAvailable/{print int($2/1024/1024)}' /proc/meminfo) GB"

for a in "${ARMS[@]}"; do
    NP="${a%%:*}"; IDS="${a#*:}"; tag=$(basename "$IDS" .ids)
    M "== 臂 $tag: 提示 $NP, 共 $(wc -l < "$IDS") token"
    speed-bench/bugmd_gates_spark.sh scoreids "$IDS" "$NP" 32 "$ENTRY" "$AMP" "$MODEL" 2>&1 | grep -a "^\[bugmd\]\|最想停\|进过 top" | sed 's/^/    /'
    bin="/tmp/bugmd/${tag}_top32_$(basename "$AMP" | sed 's/^DeepSeek-V4.1-Flash-//; s/-engine$//').bin"
    [ -s "$bin" ] || { M "★没有打分产物 $bin★"; continue; }
    M "-- $tag minp(入环位 $ENTRY)"
    python3 "$TOOLS" minp "$bin" "$IDS" "$NP" "$ENTRY" 0.02 128822 2>&1 | grep -a "入环\|出口 Σp\|偏离" | sed 's/^/    /'
    cp "$bin" "/tmp/bugmd/${tag}_top32_"*.thinkend "$(dirname "$IDS")/" 2>/dev/null
done
if [ "$WT2" = 1 ]; then
    M "== wt2 守门(引擎 裸 / +编程侧车, 教师锚按 1017 另存)"
    gguf-tools/scripts/v41_judge.sh gguf/go-onebit/g7/wt2.ids 512 --hf hf/DeepSeek-V4.1-Flash-1017 \
        "engine:$MODEL" "engine:$MODEL:$AMP" 2>&1 | grep -a "PPL\|KLD\|Same\|★\|engine" | sed 's/^/    /'
fi
