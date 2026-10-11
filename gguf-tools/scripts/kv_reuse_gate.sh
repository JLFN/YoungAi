#!/bin/bash
# kv_reuse_gate.sh — V4.1 KV 前缀复用的精确门(src/core/core_v41_kvreuse.c; 2026-10-11)。
#   用法: kv_reuse_gate.sh [模型端口 8001] [提示文件 docs/TECHNICAL.md] [提示字数 12000] [生成 token 数 48]
#   要一个开着 --trace 的 ds4-server(工作台起的模型子进程默认就开, trace 路径在 gguf/v41/night/trace/current)。
#
# 门是什么: 检查点到底存全了没有。
#   A1 = 提示 P, 温 0, 只要 1 个 token → 拿到第一个 token 的文本 t1
#   A  = 提示 P, 温 0, 要 N 个 token → 生成 id g0 g1 … (A 解码这 N 步把窗口环 / 压缩器余行 / 组数全改过)
#   B  = 提示 P+t1, 温 0, 要 N-1 个 → 应当从 A 存的检查点(P 末尾)接上, 只预填 1 个 token
#   B 的生成 id 必须逐个等于 g1 g2 …, 而且每一步整行 logits 的指纹(引擎写进 trace 的加权和)逐位相等:
#   B 那 1 个 token 的预填与 A 解码第一步是同一个 n=1 前向, 检查点漏存任何一样就对不上。只比 token 不够 —— 10-11 变异实测
#   故意不恢复压缩器余行, 200 个 token 照样全对(只错一组压缩 KV, logit 动得翻不动 argmax), 比指纹才抓得到。
#   前提要先核: B 的提示 id 必须恰好 = A 的提示 id + [g0](t1 接在 P 后面重新分词没和前面粘成别的 token), 不成立就换 P 的字数再跑。
# 判读: 打印 "PASS" / "FAIL 第 k 位分叉", 以及 B 的复用数(trace 的 "kv reuse: …" 行)。没有复用行 = 没接上, 也算 FAIL。
# 出错会怎样: 端口不对 → curl 报连不上; trace 没开 → 找不到 token ids 块, 脚本直接说。
set -uo pipefail
PORT="${1:-8001}"; FILE="${2:-docs/TECHNICAL.md}"; NCH="${3:-12000}"; N="${4:-48}"
U="http://127.0.0.1:$PORT/v1/completions"
TR=$(cat gguf/v41/night/trace/current 2>/dev/null) || { echo "找不到 gguf/v41/night/trace/current(要在仓库根目录跑, 服务要开 --trace)"; exit 1; }
P=$(head -c "$NCH" "$FILE" | iconv -c -f utf-8 -t utf-8)   # 按字节截可能切坏最后一个汉字: iconv -c 丢掉半截
# frequency_penalty 1e-30: 让请求走主机惩罚路, 引擎逐步记 logits 指纹进 trace(core_v41_sample.c); 1e-30 在 fp32 下减不动任何 logit, 顺带关投机
req() { jq -n --arg p "$1" --argjson n "$2" '{prompt:$p, raw:true, max_tokens:$n, temperature:0, frequency_penalty:1e-30}' | curl -s --noproxy '*' --max-time 900 "$U" -H 'Content-Type: application/json' -d @-; }
# 只认 "--- token ids" 头后面紧跟的两行: trace 里还记着提示原文, 原文里以 "prompt:" 开头的行会被当成 id(10-11 实撞, 中文技术文档)
last_ids() { awk '/^--- token ids:/{h=2; next} h==2{p=$0; h=1; next} h==1{g=$0; h=0} END{print p; print g}' "$TR" | sed 's/^[a-z]*: *//'; }
last_reuse() { grep -o 'kv reuse: [0-9]* of [0-9]*' "$TR" | tail -1; }
nblk() { grep -c '^--- trace: logit sums:' "$TR"; }   # 每条请求收尾最后写的就是这一行(token ids 块在它前面)
last_sums() { grep '^--- trace: logit sums:' "$TR" | tail -1 | sed 's/^--- trace: logit sums: *//; s/ ---$//'; }
# 服务端先回 HTTP、收尾时才写 id 块: 回来就读会读到上一条的块(10-11 实撞)。等块数涨了再读
wait_blk() { for _ in $(seq 1 100); do [ "$(nblk)" -gt "$1" ] && return 0; sleep 0.1; done; echo "等了 10 s trace 里没有新的 token ids 块"; exit 1; }

echo "提示: $FILE 前 $NCH 字节; 生成 $N; trace $TR"
n0=$(nblk); T1=$(req "$P" 1 | jq -j '.choices[0].text'; printf x); T1="${T1%x}"; wait_blk "$n0"   # 哨兵 x: $(…) 会吃掉结尾换行, 而 t1 常常就是 "\n"
n0=$(nblk); req "$P" "$N" > /tmp/kvgate_a.json; wait_blk "$n0"; mapfile -t A < <(last_ids); read -ra AS <<< "$(last_sums)"
B_REUSE_BEFORE=$(grep -c 'kv reuse:' "$TR")
n0=$(nblk); req "$P$T1" "$((N - 1))" > /tmp/kvgate_b.json; wait_blk "$n0"; mapfile -t B < <(last_ids); read -ra BS <<< "$(last_sums)"
read -ra AP <<< "${A[0]}"; read -ra AG <<< "${A[1]}"; read -ra BP <<< "${B[0]}"; read -ra BG <<< "${B[1]}"
echo "A: 提示 ${#AP[@]} token, 生成 ${#AG[@]}; B: 提示 ${#BP[@]} token, 生成 ${#BG[@]}; t1=$(printf %q "$T1")"
[ "${#BP[@]}" -eq $(( ${#AP[@]} + 1 )) ] && [ "${A[0]} ${AG[0]}" = "${B[0]}" ] || { echo "前提不成立: B 的提示 ≠ A 的提示 + g0(t1 重新分词粘连了), 换个提示字数再跑"; exit 2; }
[ "$(grep -c 'kv reuse:' "$TR")" -gt "$B_REUSE_BEFORE" ] || { echo "FAIL: B 没有接上检查点(trace 里没有新的 kv reuse 行)"; exit 1; }
echo "B 复用: $(last_reuse)"
for ((k = 0; k < ${#BG[@]}; k++)); do
    [ "${BG[$k]}" = "${AG[$((k + 1))]}" ] || { echo "FAIL 第 $k 位分叉: B=${BG[$k]} A=${AG[$((k + 1))]}"; echo "A: $(jq -r '.choices[0].text' /tmp/kvgate_a.json | head -c 300)"; echo "B: $(jq -r '.choices[0].text' /tmp/kvgate_b.json | head -c 300)"; exit 1; }
done
# 逐步 logits 指纹: B 第 k 步(第 0 步 = 预填那 1 个 token 之后)必须逐位等于 A 第 k+1 步 —— 比 token 严得多
[ "${#BS[@]}" -ge "${#BG[@]}" ] && [ "${#AS[@]}" -gt "${#BG[@]}" ] || { echo "FAIL: trace 里的 logits 指纹步数不够(A ${#AS[@]} / B ${#BS[@]})"; exit 1; }
for ((k = 0; k < ${#BG[@]}; k++)); do
    [ "${BS[$k]}" = "${AS[$((k + 1))]}" ] || { echo "FAIL 第 $k 步 logits 不同(token 还一样): B=${BS[$k]} A=${AS[$((k + 1))]}"; exit 1; }
done
echo "PASS: B 的 ${#BG[@]} 个生成 token 与 A 的第 2..$(( ${#BG[@]} + 1 )) 个逐个相同, 每一步整行 logits 的指纹逐位相同"
# C = B 的提示 P+t1 再发一遍(同提示复用, 10-12; 检查点此刻就是 B 的, 发 P 会因为比检查点短而整段重填): 必须整份接上一块不预填
# (trace "kv reuse: n of n"), 生成 id 与逐步指纹和 B 逐位同(第 0 步就是存下来的那行 logits, 不是重算的)
n0=$(nblk); req "$P$T1" "$((N - 1))" > /tmp/kvgate_c.json; wait_blk "$n0"; mapfile -t Cc < <(last_ids); read -ra CS <<< "$(last_sums)"; read -ra CG <<< "${Cc[1]}"
R=$(last_reuse); [ "$R" = "kv reuse: ${#BP[@]} of ${#BP[@]}" ] || { echo "FAIL: 同提示没整份接上(trace: ${R:-无})"; exit 1; }
[ "${#CG[@]}" -eq "${#BG[@]}" ] || { echo "FAIL: 同提示生成数 ${#CG[@]} ≠ B ${#BG[@]}"; exit 1; }
for ((k = 0; k < ${#CG[@]}; k++)); do
    [ "${CG[$k]}" = "${BG[$k]}" ] || { echo "FAIL 同提示第 $k 位分叉: C=${CG[$k]} B=${BG[$k]}"; exit 1; }
    [ "${CS[$k]}" = "${BS[$k]}" ] || { echo "FAIL 同提示第 $k 步 logits 不同: C=${CS[$k]} B=${BS[$k]}"; exit 1; }
done
echo "PASS: 同提示 C 整份接上($R), ${#CG[@]} 个 token 与 B 逐个相同, 每步 logits 指纹逐位相同"
