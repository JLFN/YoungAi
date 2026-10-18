#!/bin/bash
# train_ui_spark.sh — 起/停工作台主进程 ds4-train(2026-10-10)。
#   用法: train_ui_spark.sh start [端口 8000] | stop | status | restart [端口 8000] | model-restart [端口 8000]
#   主进程永远不退: 页面 http://<主机>:<端口>/ 出聊天/模型/训练/语料/记录; 模型(ds4-server)它起在 127.0.0.1:<端口+1> 当子进程并转发 /v1,
#   训练是它自己的作业线程(停模型 → 训 → 门 → 模型回来, 全在 C 里)。stop 只停主进程, 模型子进程留着(下次 start 直接接上; 要停模型用 serve_1m_spark.sh stop)。
#   model-restart = 换了 ds4-server 二进制(sync_spark.sh engine)之后用: 主进程不动、页面不断, SIGTERM 模型子进程等它卸完映射(几十秒),
#   再让主进程按 gguf/serve_pick.txt 把同一套装回来(POST /api/train/serve, 口令 = ~/.ds4/assistant/token)。不这么做子进程还是旧二进制, 改了默认值也不生效(10-15 实撞)。
#   restart = 换了 ds4-train 二进制之后用: stop + start。模型子进程不动, start 后直接接上, 不用重装模型。
#   restart 和 model-restart 都先等助理手上没有在跑/排队/等批准的任务、也没在训练: 助理循环和训练作业线程住在主进程里, 模型一停它们的请求也断,
#   直接动手会把任务掐断(10-18 换模型清单时助理正跑着一条工具任务)。
#   注意 /api/models/list 在模型跑着时由主进程转发给 ds4-server 答(它也链了 model_catalog.o): 改了模型清单要重链两个二进制, 光 restart 页面看不到变化。
#   日志 /tmp/ds4_train_ui.log。
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"; cd "$ROOT" || exit 1
PORT="${2:-8000}"
wait_idle() {
    local tok s b i
    tok=$(cat ~/.ds4/assistant/token) || { echo "没有 ~/.ds4/assistant/token"; exit 1; }
    for i in $(seq 1 100000); do
        s=$(curl -s --noproxy '*' -m 5 -H "Authorization: Bearer $tok" "http://127.0.0.1:$PORT/api/assistant/sessions" | grep -o '"state":"[a-z_]*"' | grep -vc '"idle"')
        b=$(curl -s --noproxy '*' -m 5 -H "Authorization: Bearer $tok" "http://127.0.0.1:$PORT/api/train/status" | grep -c '"busy":true')
        [ "${s:-0}" = 0 ] && [ "${b:-0}" = 0 ] && return 0
        [ $((i % 6)) = 1 ] && echo "$(date +%T) 等: 助理在跑 ${s} 个会话, 训练中 ${b}"
        sleep 10
    done
}
case "${1:-status}" in
  start)
    [ -x ./ds4-train ] || { echo "没有 ./ds4-train, 先 make ds4-train"; exit 1; }
    pgrep -x ds4-train >/dev/null && { echo "已在跑: $(pgrep -a -x ds4-train)"; exit 0; }
    nohup ./ds4-train --host 0.0.0.0 --port "$PORT" > /tmp/ds4_train_ui.log 2>&1 </dev/null &
    sleep 1
    pgrep -x ds4-train >/dev/null && echo "起了: http://$(hostname -I 2>/dev/null | awk '{print $1}'):$PORT/" || { cat /tmp/ds4_train_ui.log; exit 1; }
    ;;
  stop) pkill -x ds4-train && echo "停了(模型子进程没动)" || echo "没在跑";;
  restart)
    wait_idle
    SELF="$ROOT/gguf-tools/scripts/train_ui_spark.sh"   # 不用 $0: 从别的目录以相对路径调用时, cd 到仓库根后 $0 就指不到了
    bash "$SELF" stop; for i in $(seq 1 30); do pgrep -x ds4-train >/dev/null || break; sleep 1; done
    bash "$SELF" start "$PORT";;
  status) pgrep -a -x ds4-train || echo "没在跑"; curl -s --noproxy '*' "http://127.0.0.1:$PORT/api/train/status" 2>/dev/null; echo;;
  model-restart)
    TOK=$(cat ~/.ds4/assistant/token) || { echo "没有 ~/.ds4/assistant/token"; exit 1; }
    pgrep -x ds4-train >/dev/null || { echo "主进程没在跑, 先 start"; exit 1; }
    wait_idle
    if pkill -x ds4-server; then
        echo "SIGTERM 了 ds4-server, 等它卸映射…"
        for i in $(seq 1 60); do pgrep -x ds4-server >/dev/null || break; sleep 2; done
        pgrep -x ds4-server >/dev/null && { echo "★ds4-server 2 分钟停不掉★"; exit 1; }
        sleep 3   # 主进程的看门狗线程每秒 waitpid 一次, 收到退出才把状态放回 IDLE; 早一步 POST 会被 409
    else echo "ds4-server 没在跑, 直接装"; fi
    echo "serve: $(curl -s --noproxy '*' -m 10 -H "Authorization: Bearer $TOK" -X POST "http://127.0.0.1:$PORT/api/train/serve")"
    for i in $(seq 1 150); do
        M=$(curl -s --noproxy '*' -m 5 -H "Authorization: Bearer $TOK" "http://127.0.0.1:$PORT/api/train/status" | grep -o '"mode":"[a-z_]*"')
        [ "$M" = '"mode":"serving"' ] && { echo "装好($((i * 4)) s 内): $(pgrep -a -x ds4-server | cut -c1-80)"; exit 0; }
        sleep 4
    done
    echo "★10 分钟没装好: $(curl -s --noproxy '*' -m 5 -H "Authorization: Bearer $TOK" "http://127.0.0.1:$PORT/api/train/status" | cut -c1-300)★"; exit 1;;
  *) echo "用法: $0 start [端口] | stop | status | restart [端口] | model-restart [端口]"; exit 1;;
esac
