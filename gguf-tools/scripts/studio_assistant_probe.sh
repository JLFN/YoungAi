#!/bin/bash
# studio_assistant_probe.sh — 从工作台接口走助理的阶段 1 门(docs/assistant.md §11; 2026-10-11)。
#   用法: studio_assistant_probe.sh <memory|edit|approve|web|reconnect|all> [页面端口 8000]
#         studio_assistant_probe.sh ask "<任意一句话>" [页面端口 8000]   —— 新开会话原样发这句, 跑完打印对话记录(真实任务试跑用)
#   全程只打主进程 ds4-train 的 /api/assistant/*(与浏览器页面同一套接口, 带同样的 X-DS4-Assistant 头), 不碰内部文件 ——
#   验的就是用户在页面上走的那条路。每段把事件流原样转成对话记录(用户/助理/工具/审批/结果)进日志, 判读看原文。
#   memory   : "记住我叫…住在…" → 新开会话问"我住哪" → 回答里应有那个城市; profile.md 多一行带日期会话号
#   edit     : 让它在工作区建文件再改一处 → 结果卡显示 created/edited, .trash 里有旧版本
#   approve  : 让它读工作区外的 /etc/hostname → 出审批 → 拒 → 再来一次选"总是" → 放行; 审计里 denied 与 always 各一行
#   web      : 让它搜一条新闻并打开第一个结果(火狐无头 + BiDi)
#   reconnect: 运行中从第 0 行与中途行号各接一次事件流, 两份的公共部分逐行相同
#   sites    : 不经模型, ./ds4_asst_test --visit 直接开一串常见站(指纹页 + 电商/社区/搜索/Cloudflare 挑战页), 看哪些被反爬拦(10-11);
#              每页打印开头 600 字, 拦没拦看原文("验证/频繁/403/captcha/Just a moment"之类)。要在仓库根目录跑, 先 make ds4_asst_test
#   日志 /tmp/studio_asst_<MMDDHHMM>.log。
set -uo pipefail
CASE="${1:?memory|edit|approve|web|reconnect|sites|all|ask}"
if [ "$CASE" = ask ]; then ASK="${2:?ask 要一句话}"; PORT="${3:-8000}"; else PORT="${2:-8000}"; fi
U="http://127.0.0.1:$PORT/api/assistant"
LOG="/tmp/studio_asst_$(date +%m%d%H%M).log"
H=(-H 'Content-Type: application/json' -H 'X-DS4-Assistant: 1')
C() { curl -s --noproxy '*' --max-time 30 "$@"; }
say() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }

send() {   # send <文本> [会话] → 打印会话 id
    local body; body=$(jq -cn --arg t "$1" --arg s "${2:-}" '{text:$t, session:$s}')
    C "${H[@]}" -d "$body" "$U/send" | jq -r '.session // ("ERR " + (.error|tostring))'
}
state() { C "$U/sessions" | jq -r --arg s "$1" '.[] | select(.id==$s) | .state'; }
events() { curl -s --noproxy '*' --max-time "${2:-2}" "$U/stream?session=$1&from=${3:-0}" | sed -n 's/^data: //p'; }
# 等到不再 running/queued; 返回最终状态(idle / waiting_approval)
wait_run() {
    local s st t0=$SECONDS
    s="$1"
    while :; do
        st=$(state "$s")
        [ "$st" = "idle" ] || [ "$st" = "waiting_approval" ] && { echo "$st"; return; }
        [ $((SECONDS - t0)) -gt 3600 ] && { echo "timeout"; return; }   # 助理默认开思考后(10-12)真实任务 16 分钟量级, 900 s 不够(实撞: 任务跑完了探针先报 timeout)
        sleep 1
    done
}
transcript() {   # 事件 → 对话记录(正文增量拼起来)
    events "$1" 2 | jq -r -s '
      reduce .[] as $e ({out:[], buf:""};
        if $e.ev=="text" then .buf += $e.d
        else (if .buf!="" then .out += ["助理: " + .buf] | .buf="" else . end)
          | if $e.ev=="user" then .out += ["用户: " + $e.text]
            elif $e.ev=="tool" then .out += ["  工具 " + $e.name + " " + $e.args]
            elif $e.ev=="approval" then .out += ["  审批? " + $e.name + " 目标=" + $e.target + " 原因=" + $e.reason + " 仅这次=" + ($e.once_only|tostring)]
            elif $e.ev=="tool_end" then .out += ["  结果 ok=" + ($e.ok|tostring) + " 决定=" + $e.decision + " " + ($e.ms|tostring) + "ms: " + ($e.result[0:300]|gsub("\n";" ⏎ "))]
            elif $e.ev=="error" then .out += ["  错误: " + $e.msg]
            elif $e.ev=="usage" then .out += ["  用量 prompt=" + ($e.prompt|tostring) + " completion=" + ($e.completion|tostring)]
            elif $e.ev=="end" then .out += ["  结束: " + $e.reason]
            else . end end)
      | (if .buf!="" then .out += ["助理: " + .buf] else . end) | .out[]' | tee -a "$LOG"
}
pending_id() { events "$1" 2 | jq -r -s '[.[] | select(.ev=="approval")] | last | .id'; }
approve() { C "${H[@]}" -d "$(jq -cn --arg s "$1" --arg i "$2" --arg d "$3" '{session:$s,id:$i,decision:$d}')" "$U/approve"; echo; }

run() {   # run <文本> [会话] → 跑完打印记录, 返回会话 id 于全局 SID
    SID=$(send "$1" "${2:-}")
    say "会话 $SID ← $1"
    case "$SID" in ERR*) say "$SID"; return 1;; esac
    say "状态: $(wait_run "$SID")"
}

case_memory() {
    say "== memory =="
    run "记住：我叫测试员小周，住在宁波，周三晚上不排事。"; transcript "$SID"
    run "我住在哪个城市？只回答城市名。"; transcript "$SID"
    say "profile.md:"; C "$U/memory" | jq -r .profile | tee -a "$LOG"
}
case_edit() {
    say "== edit =="
    run "在工作区建一个文件 probe/a.txt，内容三行：alpha、beta、gamma。"; local s="$SID"; transcript "$s"
    run "把 probe/a.txt 里的 beta 改成 BETA，然后读出来给我看。" "$s"; transcript "$s"
}
case_approve() {
    say "== approve =="
    run "读一下 /etc/hostname 这个文件的内容告诉我。"; local s="$SID" id
    id=$(pending_id "$s"); say "审批 id=$id → deny"; approve "$s" "$id" deny | tee -a "$LOG"
    say "状态: $(wait_run "$s")"; transcript "$s"
    run "我同意了，再读一次 /etc/hostname。" "$s"
    id=$(pending_id "$s"); say "审批 id=$id → always"; approve "$s" "$id" always | tee -a "$LOG"
    say "状态: $(wait_run "$s")"; transcript "$s"
    say "审计最近 4 行:"; C "$U/activity" | jq -c '.audit[0:4][] | {tool, decision, ok, args}' | tee -a "$LOG"
}
case_web() {
    say "== web =="
    run "用网页搜索查一下“DeepSeek V4”最新消息，打开第一个结果，用三句话总结。"; transcript "$SID"
}
case_reconnect() {
    say "== reconnect =="
    SID=$(send "从 1 数到 300，每个数字单独一行，不要省略。")
    say "会话 $SID"; sleep 3   # 解码 ~40 token/s, 300 行要十几秒: 3 s 时一定还在跑
    local a b; a=$(events "$SID" 1 0 | wc -l); say "运行中已有 $a 行(状态 $(state "$SID"))"
    wait_run "$SID" >/dev/null
    events "$SID" 2 0 > /tmp/studio_asst_full.$$; events "$SID" 2 "$a" > /tmp/studio_asst_tail.$$
    b=$(tail -n +"$((a + 1))" /tmp/studio_asst_full.$$ | cmp - /tmp/studio_asst_tail.$$ && echo 同 || echo 不同)
    say "从第 $a 行接上 vs 全量第 $a 行之后: $b(全量 $(wc -l < /tmp/studio_asst_full.$$) 行)"
    rm -f /tmp/studio_asst_full.$$ /tmp/studio_asst_tail.$$
}

SITES=(https://bot.sannysoft.com/ https://item.jd.com/100236761944.html "https://search.jd.com/Search?keyword=DGX%20Spark"
    "https://s.taobao.com/search?q=DGX%20Spark" "https://www.zhihu.com/search?q=DGX%20Spark" "https://search.smzdm.com/?s=DGX%20Spark"
    "https://search.bilibili.com/all?keyword=DGX%20Spark" "https://www.baidu.com/s?wd=DGX%20Spark" https://www.xiaohongshu.com/explore
    "https://www.amazon.com/s?k=DGX+Spark" "https://www.newegg.com/p/pl?d=dgx+spark" "https://www.reddit.com/search/?q=dgx+spark" https://nowsecure.nl/)
case_sites() { say "== sites =="; ./ds4_asst_test --visit "${SITES[@]}" 2>&1 | tee -a "$LOG"; }

say "日志 $LOG; 接口 $U; 状态 $(C "$U/status")"
case "$CASE" in
    memory) case_memory;; edit) case_edit;; approve) case_approve;; web) case_web;; reconnect) case_reconnect;; sites) case_sites;;
    all) case_memory; case_edit; case_approve; case_web; case_reconnect;;
    ask) say "== ask =="; run "$ASK"; transcript "$SID";;
    *) echo "未知: $CASE"; exit 1;;
esac
