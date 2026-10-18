#!/bin/bash
# v41_base_quant_chain_spark.sh — 新底座按现役配方量化成部署 GGUF 的全链(2026-10-17, 用户令"开启新模型 DeepSeek-V4.1-Flash-1017 的量化")。
#
# 只串既有入口, 数值全在各自的程序里(与 v41_fincal_chain_spark.sh 同一套纪律; 那条链是金融校准专用, 带 S0/S1 取料段, 这里是零语料):
#   STOP  停工作台的模型子进程 ds4-server(它装着 ~95 GB, 量化与教师锚都要整机内存; 主进程 ds4-train 永不退, 不碰)
#   B1    v41_quantize_spark.sh --hf <新底座> …现役 v3 配方…  --judge-set all
#         配方 = 09-21 发车串原样: 骨架 q4_K + 三塔 VQ + 路由专家 VQ dim8 nc4096, 每层一本共享码本 E4M3, 最浅 14 层 13 位, 零语料
#         判决 = 文件态三尺(金融 j 8192 / 八域 j 8192 / wt2 512), 教师锚用【新底座】重算(v41_judge.sh --hf 带 _<目录名> 后缀另存)
#   G     v41_to_gguf → gguf/v41/<底座名>-vq8sh14-q4k-mtpnative.gguf(engram/tokenizer 常量复用 gguf/v41/consts: 新旧 config/tokenizer/index 逐字节同, 只换了权重)
#   C     v41_engine_parity_spark.sh --gguf <新> wt2 512: 引擎 vs 文件态学生, 同一文件, KL ≤ 0.07 才算接线对
#   SMOKE ./ds4 -m <新> 温 0 生成 96 token(原始输出原样落日志)
#   RESTORE 不论成败, 退出时按 gguf/serve_pick.txt 把原来那套模型装回工作台(POST /api/models/load, 与页面同路)
# 停车规则: 任一段非 0 就停在那一段(半成品往下传只出假数); 每段开头打 [chain] 时间戳, 尾行 CHAIN_EXIT <rc>。
# 看门狗: 各段脚本自带(available < 8 GB 杀); 本脚本只在 STOP 后核一次整机余量。
# 用法(spark 本机, nohup): v41_base_quant_chain_spark.sh --hf hf/DeepSeek-V4.1-Flash-1017 [--from STOP|B1|G|C|SMOKE] [--no-restore]
#   出错会怎样: 工作台模型子进程被停后, 页面聊天会答"没有装着的模型"直到 RESTORE 段把它装回来; --no-restore 则要自己去模型页点"加载"。
set -uo pipefail
ROOT="$HOME/ds4-main"; cd "$ROOT" || exit 1
HF=""; FROM="STOP"; RESTORE=1; JSET="all"
while [ $# -gt 0 ]; do case "$1" in
  --hf) HF="${2:?--hf 要目录}"; shift 2;;
  --from) FROM="${2:?--from 要段名}"; shift 2;;
  --judge-set) JSET="${2:?--judge-set 要 fin|eight|wt2|all}"; shift 2;;   # 10-17 用户令"删除八域": 续跑只补缺的尺; C 段只要 wt2 那份学生
  --no-restore) RESTORE=0; shift;;
  --no-restore) RESTORE=0; shift;;
  *) echo "★不认识的参数 $1★"; exit 2;;
esac; done
[ -n "$HF" ] || { echo "★要 --hf <出厂权重目录>★"; exit 2; }
[ -s "$HF/config.json" ] && [ -s "$HF/model.safetensors.index.json" ] || { echo "★$HF 不是出厂权重目录★"; exit 2; }
HF="$(cd "$HF" && pwd)"; BASE="$(basename "$HF")"
OUTDIR="gguf/v41/$BASE-vq8sh14-q4k"
GG="gguf/v41/$BASE-vq8sh14-q4k-mtpnative.gguf"
WT2="gguf/go-onebit/g7/wt2.ids"
PORT=8000
M(){ echo "[chain $(date '+%m-%d %H:%M:%S')] $*"; }
die(){ M "★$*★"; exit 1; }   # CHAIN_EXIT 由 EXIT trap 统一打(先装回模型再报退出码)
stage_ge(){ local order="STOP B1 G C SMOKE"; local a=${order%%$1*}; local b=${order%%$FROM*}; [ ${#a} -ge ${#b} ]; }
for f in "$WT2" gguf/v41/consts/engram_consts.bin gguf/v41/consts/tokenizer_consts.bin gguf/go-onebit/vqfin41/vqhalf_j.ids gguf/go-onebit/vqhalf/vqhalf_j.ids; do
    [ -e "$f" ] || die "缺 $f"
done
# 48 片必须齐: 量化器按 index.json 找张量, 缺一片是在某一层中途才报错(白跑前面的层)
nsh=$(ls "$HF"/model-000*-of-00048.safetensors 2>/dev/null | wc -l); [ "$nsh" = 48 ] || die "$HF 只有 $nsh/48 片"
# 常量复用的前提: 新旧 config/tokenizer 逐字节同(engram 哈希常量只从 tokenizer 与 inference/config.json 算, 词表只从 tokenizer.json 算)
for f in config.json tokenizer.json inference/config.json; do
    cmp -s "$HF/$f" "hf/DeepSeek-V4.1-Flash/$f" || die "$f 与旧底座不同, gguf/v41/consts 不能复用, 先用 v41_engram_consts.py / v41_tokenizer_consts.py 重算"
done
avail_gb(){ free -g | awk '/^内存|^Mem/{print $7}'; }

# ---- RESTORE: 退出时装回原来那套(serve_pick.txt 第 1/2 行是绝对路径, 接口要仓库相对路径) ----
restore_model() {
    [ "$RESTORE" = 1 ] || { M "--no-restore: 模型子进程不装回, 自己去模型页点加载"; return; }
    [ -s gguf/serve_pick.txt ] || { M "★没有 serve_pick.txt, 不知道装回哪套★"; return; }
    local pg pz tok body resp
    pg=$(sed -n 1p gguf/serve_pick.txt); pz=$(sed -n 2p gguf/serve_pick.txt)
    pg="${pg#"$ROOT"/}"; pz="${pz#"$ROOT"/}"; [ -n "$pz" ] || pz=none
    tok=$(cat ~/.ds4/assistant/token 2>/dev/null || true)
    body=$(printf '{"gguf":"%s","sidecar":"%s"}' "$pg" "$pz")
    resp=$(curl -s --noproxy '*' --max-time 30 ${tok:+-H "Authorization: Bearer $tok"} -H 'Content-Type: application/json' -d "$body" "http://127.0.0.1:$PORT/api/models/load")
    M "RESTORE 装回 $pg + $pz → $resp"
}
trap 'rc=$?; restore_model; echo "CHAIN_EXIT $rc"' EXIT

if stage_ge STOP; then
  M "STOP: 停工作台模型子进程(主进程 ds4-train 不动)"
  pgrep -x ds4-train >/dev/null || M "★ds4-train 主进程没在跑, RESTORE 段会装不回; 先 train_ui_spark.sh start★"
  if pgrep -x ds4-server >/dev/null; then
      pkill -x -TERM ds4-server
      for _ in $(seq 1 60); do pgrep -x ds4-server >/dev/null || break; sleep 2; done
      pgrep -x ds4-server >/dev/null && { pkill -x -KILL ds4-server; sleep 5; }
      pgrep -x ds4-server >/dev/null && die "ds4-server 杀不掉"
  fi
  sleep 5
  av=$(avail_gb); M "整机 available ${av} GB"
  [ "${av:-0}" -ge 90 ] || die "停了模型子进程 available 仍只有 ${av} GB(< 90), 机器上还有别的大户, 不发车"
  # 管道的退出码是 head 的(恒 0), 所以先用 pgrep 本身判有没有, 再列给人看
  if pgrep -f "quantize/v41_quantiz[e]|v41_teacher[.]py|v41_amp_ru[n] |ds[4] --cuda" >/dev/null; then
      pgrep -a -f "quantize/v41_quantiz[e]|v41_teacher[.]py|v41_amp_ru[n] |ds[4] --cuda" | head -3
      die "机器非空(实例锁): 上面这些在跑"
  fi
fi

if stage_ge B1; then
  M "B1 全量量化 $BASE → $OUTDIR (现役 v3 配方, 零语料) + 文件态三尺(教师锚按新底座重算)"
  df -h "$ROOT" | tail -1
  bash gguf-tools/scripts/v41_quantize_spark.sh --hf "$HF" --out "$OUTDIR" \
      --skel q4k --mtp-vq --vq-nc 4096 --vq-nc-layers 0:14=8192 --vq-shared-cb --vq-cb-fp8 \
      --stats-out "$OUTDIR/stats.txt" --judge-set "$JSET" || die "B1 失败(见 $OUTDIR/quantize_log.txt)"
fi

if stage_ge G; then
  M "G 转 GGUF → $GG"
  make -C gguf-tools v41_to_gguf >/tmp/basechain_make_g.log 2>&1 || die "v41_to_gguf 编译失败(见 /tmp/basechain_make_g.log)"
  gguf-tools/quantize/v41_to_gguf "$OUTDIR" gguf/v41/consts/engram_consts.bin gguf/v41/consts/tokenizer_consts.bin "$GG" "$HF" 2>&1 | tee /tmp/basechain_g.log | tail -8
  [ "${PIPESTATUS[0]}" = 0 ] && [ -s "$GG" ] || die "G 转换失败(见 /tmp/basechain_g.log)"
  sz=$(stat -c %s "$GG"); M "GGUF $sz B = $(awk -v s="$sz" 'BEGIN{printf "%.3f", s/1e9}') GB(现役 v3 同配方 113.557 GB)"
  grep -E "engram\] L" /tmp/basechain_g.log | head -2   # engram 表路径必须指向新底座的 47/48 片
  # 视觉塔侧车(10-13 用户定: 不进主文件, 引擎 --vision-gguf 第二映射; 原件 BF16 不压, 现役那份 971,066,016 B)
  VG="gguf/v41/$BASE.vision.gguf"
  gguf-tools/quantize/v41_to_gguf vision "$OUTDIR" "$HF" "$VG" 2>&1 | tail -3
  [ "${PIPESTATUS[0]}" = 0 ] && [ -s "$VG" ] || die "视觉侧车转换失败"
  M "视觉侧车 $VG $(stat -c %s "$VG") B(现役 971,066,016 B)"
fi

if stage_ge C; then
  M "C 引擎对拍 wt2 512: 引擎 vs 文件态 Python 学生(同一文件)"
  STU=$(ls -t gguf/v41judge/stu_g7_wt2_n512_file_"$(basename "$OUTDIR")"*.bin 2>/dev/null | head -1)
  [ -s "$STU" ] || die "C 缺文件态 wt2 学生 logits(B1 的 --judge-set all 该落它)"
  bash gguf-tools/scripts/v41_engine_parity_spark.sh "$WT2" 512 "$STU" --engram --gguf "$ROOT/$GG" 2>&1 | tee /tmp/basechain_c.log | grep -E "KLD|Same|PPL|★"
  [ "${PIPESTATUS[0]}" = 0 ] || die "C 对拍失败(见 /tmp/basechain_c.log)"
fi

if stage_ge SMOKE; then
  M "SMOKE 温 0 生成 96 token(原始输出原样落 /tmp/basechain_smoke.log)"
  ./ds4 -m "$GG" --cuda --mem-budget-mb 40000 -p "请用三句话解释什么是市盈率。" -n 96 --temp 0 --seed 1 >/tmp/basechain_smoke.log 2>&1 \
      || die "SMOKE 失败(见 /tmp/basechain_smoke.log)"
  tail -12 /tmp/basechain_smoke.log
fi
M "全链收工: $GG"
