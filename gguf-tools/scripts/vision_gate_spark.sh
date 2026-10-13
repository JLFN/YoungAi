#!/bin/bash
# vision_gate_spark.sh — 视觉塔接入的真机门(2026-10-13, spark)。四步, 每步原始输出落 gguf/v41/vision/gate_<时间>/:
#   ① 金标门: ./ds4 --image 图 --vision-golden 金标(core_v41_vision_gate.c 四道比: 尺寸账 / 像素 / patch / 塔逐行 cos)
#   ② 文本不变门: 同一提示温 0 贪心 64 token, 带 / 不带 --vision-gguf 两趟, 输出必须逐字节同(侧车不许碰文本路)
#   ③ 看图: 温 0 贪心, 真图提问, 回答原样打印(判读交给人: "每次给看原始输出")
#   ④ 不留进程: 每趟都是一次性 ./ds4, 跑完即退; 生产模型子进程由调用方先停(serve_1m_spark.sh stop), 门过了再 pick + 起
# 用法: vision_gate_spark.sh [主GGUF] [侧车GGUF] [zchain目录]   默认取 gguf/serve_pick.txt 的前两行 + gguf/v41/DeepSeek-V4.1-Flash.vision.gguf
# 内存: 每趟装一次 110 GB 模型(约 100 s); 跑前要求 MemAvailable ≥ 100 GB(生产没停就拒), 与 serve_1m_spark.sh 同一道起跑清单。
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"; cd "$ROOT" || exit 1
PICK=gguf/serve_pick.txt
MAIN="${1:-$(sed -n 1p $PICK)}"; VIS="${2:-gguf/v41/DeepSeek-V4.1-Flash.vision.gguf}"; ZCH="${3:-$(sed -n 2p $PICK)}"
BUD=110000
OUT="gguf/v41/vision/gate_$(date +%m%d_%H%M)"; mkdir -p "$OUT"
die() { echo "★$*★" | tee -a "$OUT/gate.log"; exit 1; }
log() { echo "$*" | tee -a "$OUT/gate.log"; }
[ -f "$MAIN" ] || die "主 GGUF 不存在: $MAIN"; [ -f "$VIS" ] || die "侧车不存在: $VIS"; [ -d "$ZCH" ] || die "zchain 目录不存在: $ZCH"
if pgrep -f "ds4-serve[r] --cuda" >/dev/null; then die "生产模型子进程还在跑, 先 gguf-tools/scripts/serve_1m_spark.sh stop"; fi
AV=$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo); [ "$AV" -ge 100000 ] || die "MemAvailable ${AV} MB < 100000, 装不下第二份模型"
log "[gate] 主 $MAIN  侧车 $VIS  链 $ZCH  产物 $OUT"
COMMON=(--cuda -m "$MAIN" --zchain "$ZCH" --mem-budget-mb $BUD --no-dspark)

# ① 金标门(两张图)
for g in panel:review-market-panel.png img:img.jpeg; do
    n=${g%%:*}; img=${g#*:}; gold="gguf/v41/vision/golden_$n.bin"
    [ -f "$gold" ] || { log "跳过 $n: 没有金标 $gold"; continue; }
    log "== ① 金标门 $n"
    ./ds4 "${COMMON[@]}" --vision-gguf "$VIS" --image "$img" --vision-golden "$gold" > "$OUT/golden_$n.txt" 2>&1
    grep -E '^[①②③④⑤]|vision-golden|★' "$OUT/golden_$n.txt" | tee -a "$OUT/gate.log"
done

# ② 文本不变门
P="请用三句话解释什么是边际成本。"
log "== ② 文本不变门(温 0, 64 token)"
./ds4 "${COMMON[@]}" --temp 0 -n 64 -p "$P" > "$OUT/text_base.txt" 2> "$OUT/text_base.err"
./ds4 "${COMMON[@]}" --vision-gguf "$VIS" --temp 0 -n 64 -p "$P" > "$OUT/text_vis.txt" 2> "$OUT/text_vis.err"
if cmp -s "$OUT/text_base.txt" "$OUT/text_vis.txt"; then log "文本逐字节同 ✓"; else log "★文本不同★(侧车碰了文本路)"; diff "$OUT/text_base.txt" "$OUT/text_vis.txt" | head -5 | tee -a "$OUT/gate.log"; fi
log "--- 文本原样: $(head -c 300 "$OUT/text_vis.txt")"

# ③ 看图(原样输出)
for g in review-market-panel.png img.jpeg; do
    log "== ③ 看图 $g"
    ./ds4 "${COMMON[@]}" --vision-gguf "$VIS" --temp 0 -n 200 --image "$g" -p "这张图里有什么？把你看到的文字和数字都列出来。" > "$OUT/see_${g%.*}.txt" 2> "$OUT/see_${g%.*}.err"
    grep -E '^\[v41\] 图|视觉塔已接线' "$OUT/see_${g%.*}.err" | tee -a "$OUT/gate.log"
    log "--- 回答原样:"; cat "$OUT/see_${g%.*}.txt" | tee -a "$OUT/gate.log"; echo | tee -a "$OUT/gate.log"
done
log "[gate] 完成, 产物 $OUT"
