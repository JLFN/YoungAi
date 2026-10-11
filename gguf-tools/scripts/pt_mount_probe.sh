#!/usr/bin/env bash
# pt_mount_probe.sh — 挂 ③ 看真输出(2026-10-11): 经工作台 :8000 的 C 接口热切 ③, 对同一组问题温 0 作答, 部署态 / 各 ③ 并排落盘。
#   用法: pt_mount_probe.sh <问题文件(一行一问)> <输出目录> [③目录(相对仓库根, 可多个; 空串 "" = 只挂 ②)]...
#   例:   pt_mount_probe.sh gguf-tools/data/posttrain/youngai_facts.probe.txt /tmp/mp "" gguf/v41/posttrain/kd-xxx/ckpt_e03
# 为什么要它: 留出损失/wt2 都是 teacher-forced 的数, 用户要看的是"挂上去到底答对没"(10-11: "看看实际挂上后训练输出内容究竟对不对")。
# 怎么切: POST /api/models/plugins {zchain: 当前 ②, posttrain: ③}, 服务在两条请求之间换, 轮询 /api/models/list 的 loaded.posttrain 到位才问。
# 出错会怎样: 切换被拒(base.fnv 对不上/路径不合法)直接打错误退出, 不会拿着没换成的模型问一遍冒充。跑完切回只挂 ②。
set -u
Q="${1:?问题文件}"; OUT="${2:?输出目录}"; shift 2
PORT="${PORT_DS4_TRAIN:-8000}"; U="http://127.0.0.1:$PORT"
mkdir -p "$OUT"
cur() { curl -s --noproxy '*' "$U/api/models/list?hub=0"; }
ZCH=$(cur | python3 -c 'import json,sys; print(json.load(sys.stdin)["loaded"]["zchain"])')
[ -n "$ZCH" ] || { echo "★没有装着的模型★"; exit 1; }
switch_to() {   # $1 = ③ 相对路径("" = 卸)
    local want="$1" r
    r=$(curl -s --noproxy '*' -X POST "$U/api/models/plugins" -H 'Content-Type: application/json' -d "{\"zchain\":\"$ZCH\",\"posttrain\":\"$want\"}")
    case "$r" in *'"ok":true'*) ;; *'已经是这一套'*) ;; *) echo "★切换被拒: $r★"; exit 1;; esac
    for i in $(seq 1 60); do
        local st; st=$(cur | python3 -c 'import json,sys; r=json.load(sys.stdin); print(("1" if r["switch"]["pending"] else "0"), r["loaded"]["posttrain"], r["switch"]["error"])')
        case "$st" in "0 $want "*) return 0;; "0 $want") return 0;; esac
        sleep 2
    done
    echo "★切换 60 次轮询没到位: $(cur | head -c 300)★"; exit 1
}
ask() {   # $1 = 问题 → 答案文本(温 0, 不思考, 上限 400 token)
    python3 - "$U" "$1" <<'EOF'
import json,sys,urllib.request
u,q=sys.argv[1],sys.argv[2]
body=json.dumps({"model":"deepseek-chat","messages":[{"role":"user","content":q}],"temperature":0,"max_tokens":400}).encode()
req=urllib.request.Request(u+"/v1/chat/completions",data=body,headers={"Content-Type":"application/json"})
r=json.load(urllib.request.urlopen(req,timeout=600))
print(r["choices"][0]["message"]["content"].strip())
EOF
}
for pt in "$@"; do
    tag=$( [ -z "$pt" ] && echo base || basename "$(dirname "$pt")" | sed 's/^kd-//' )_$( [ -z "$pt" ] && echo "" || basename "$pt" )
    switch_to "$pt"
    f="$OUT/$tag.txt"; : > "$f"
    echo "== ③=${pt:-无(只挂 ②)} → $f"
    n=0
    while IFS= read -r q; do
        [ -n "$q" ] || continue; n=$((n+1))
        a=$(ask "$q")
        printf '#%d 问: %s\n答: %s\n\n' "$n" "$q" "$a" >> "$f"
        printf '#%d %s\n   %s\n' "$n" "$q" "$(printf '%s' "$a" | tr '\n' ' ' | cut -c1-200)"
    done < "$Q"
done
switch_to ""
echo "== 切回只挂 ②; 产物 $OUT/*.txt"
