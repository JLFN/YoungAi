#!/bin/bash
# posttrain_pack_jsonl.sh — 把"问句 + 代码文件"拼成后训练能吃的 messages jsonl(2026-10-17, 鹈鹕 SVG 语料第一次用)。
#   用法: posttrain_pack_jsonl.sh <items.tsv> <out.jsonl> [编号 ...]
#   items.tsv 每行三列(制表符分隔): 编号  用户问句  回答开头那句话; 回答正文 = 同目录下 <编号>.a.html 或 <编号>.a.svg 的原文,
#   拼成  开头那句 + 空行 + ```html(或 svg) 围栏里的文件原文。不给编号就把 tsv 里的全打进去; 给了就只打那几条(训练/留出分两份用)。
#   为什么不直接手写 jsonl: SVG 里全是引号和换行, 手工转义必错, 而且代码文件单独放着才能用浏览器打开看效果(语料好不好看先用眼睛判)。
#   ★训练器 maxlen 1024(含提示), 每条回答要 ≤ ~960 token; 数 token 用 spark 上 ./ds4 --dump-tokens -m <V4.1 gguf> --prompt-file <文件>★
set -euo pipefail
TSV="${1:?items.tsv}"; OUT="${2:?out.jsonl}"; shift 2
DIR="$(cd "$(dirname "$TSV")" && pwd)"
: > "$OUT"
while IFS=$'\t' read -r id q intro; do
    [ -z "$id" ] && continue
    if [ $# -gt 0 ]; then case " $* " in *" $id "*) ;; *) continue;; esac; fi
    f="$DIR/$id.a.html"; lang=html
    [ -f "$f" ] || { f="$DIR/$id.a.svg"; lang=svg; }
    [ -f "$f" ] || { echo "缺 $DIR/$id.a.html / .svg" >&2; exit 1; }
    jq -cn --arg q "$q" --arg intro "$intro" --arg lang "$lang" --rawfile code "$f" \
        '{messages:[{role:"user",content:$q},{role:"assistant",content:($intro+"\n\n```"+$lang+"\n"+$code+"```")}]}' >> "$OUT"
done < "$TSV"
echo "$OUT: $(wc -l < "$OUT") 行"
