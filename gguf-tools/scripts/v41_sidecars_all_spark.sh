#!/bin/bash
# v41_sidecars_all_spark.sh — 一个基座的全部领域侧车按序解(2026-10-17, 用户令"编程跑完把其他四个域也全部接上")。
#
# 只是 amp_campaign.sh sidecar 段外面的那个 for 循环(它自己的注释就写着"三个域串行跑就在外面一个 for 循环"), 每个域:
#   ① grrb 解侧车 + 本域判决份五指标 → ② 同判决份对照裸基座 + 盘上其他已完成的本基座侧车 → ③ wt2 512 守门
# 串行不并行: 解算器与引擎学生各要整机(--mem-budget-mb 40000 --weight-cache-mb 88000), 两路同时跑就是双装基座(铁律禁)。
# 用法(spark 本机, nohup): v41_sidecars_all_spark.sh --gguf <基座.gguf> --hf <出厂权重目录> [域...]
#   域默认 law med sci fin41(编程由用户先单独发车, 不在默认表里; 要补就把 code 写进域表)。
#   先等机上正在跑的 amp_campaign.sh / v41_judge.sh / v41_amp_run 退完再开(不抢机器, 也不与它们双装)。
# 停车规则: 任一域失败就停(exit 1), 不跳到下一个 —— 少一个侧车必须让人看见, 不许"跑完了"却缺一格。
set -uo pipefail
ROOT="$HOME/ds4-main"; cd "$ROOT" || exit 1
GG=""; HF=""; DOMS=()
while [ $# -gt 0 ]; do case "$1" in
  --gguf) GG="${2:?--gguf 要文件}"; shift 2;;
  --hf) HF="${2:?--hf 要目录}"; shift 2;;
  -*) echo "★不认识的参数 $1★"; exit 2;;
  *) DOMS+=("$1"); shift;;
esac; done
[ -s "$GG" ] && [ -s "$HF/config.json" ] || { echo "★要 --gguf <存在的 gguf> 与 --hf <出厂权重目录>★"; exit 2; }
[ ${#DOMS[@]} -gt 0 ] || DOMS=(law med sci fin41)
M(){ echo "[sidecars $(date '+%m-%d %H:%M:%S')] $*"; }
BUSY="amp_campaign[.]sh|v41_judge[.]sh|v41_amp_ru[n] |v41_teacher[.]py|ds[4] -m"
if pgrep -f "$BUSY" >/dev/null; then
    M "机上还有反修/判决在跑, 等它退完再开:"; pgrep -a -f "$BUSY" | head -3 | cut -c1-160
    while pgrep -f "$BUSY" >/dev/null; do sleep 60; done
    M "机器空了"
fi
# 延后换版: amp_campaign.sh 正在跑时不能覆盖(bash 按偏移边读边执行, 09-13 实撞), Mac 侧把新版先放成 .new, 这里等机器空了再换名。
NEW="gguf-tools/scripts/amp_campaign.sh.new"
[ -s "$NEW" ] && { mv -f "$NEW" gguf-tools/scripts/amp_campaign.sh; M "amp_campaign.sh 已换成 .new 那版"; }
for d in "${DOMS[@]}"; do
    M "===== 域 $d: amp_campaign.sh --profile $d --gguf $GG --hf $HF sidecar ====="
    bash gguf-tools/scripts/amp_campaign.sh --profile "$d" --gguf "$GG" --hf "$HF" sidecar || { M "★域 $d 失败, 停车★"; echo "SIDECARS_EXIT 1"; exit 1; }
    M "域 $d 收工"
done
M "全部域收工: ${DOMS[*]}"; echo "SIDECARS_EXIT 0"
