#!/usr/bin/env bash
# tunnel_try.sh — 对正在跑的工作台(ds4-train)重开一次穿透, 逐步打印走到哪, 最后打印穿透状态。只调页面同一套接口, 不改任何设置。
# 和 tunnel_e2e.sh 的区别: 那个是从零起一个测试工作台、跑全套用例(会试错密码、传测试证书); 这个只对生产工作台"关 → 开"一次, 拿来真服务器上验证。
# 用法: tests/net/tunnel_try.sh <工作台登录用户名> <密码> [工作台地址 http://127.0.0.1:8000]
set -u
U=$1 P=$2 BASE=${3:-http://127.0.0.1:8000}
c() { curl -s --noproxy "*" -m 30 "$@"; }
S=$(c -X POST -H 'X-DS4-Studio: 1' -H 'Content-Type: application/json' --data "{\"user\":\"$U\",\"password\":\"$P\"}" "$BASE/api/auth/login" | sed -n 's/.*"session":"\([^"]*\)".*/\1/p')
[ -n "$S" ] || { echo "登录失败"; exit 1; }
api() { c -X POST -H "X-DS4-Session: $S" -H 'Content-Type: application/json' --data "$2" "$BASE/api/net/$1" > /dev/null; }
api tunnel '{"on":false}'
api tunnel '{"on":true}'
last=""
for _ in $(seq 600); do
  st=$(c -H "X-DS4-Session: $S" "$BASE/api/net/status")
  step=$(echo "$st" | sed -n 's/.*"step":"\([^"]*\)".*/\1/p')
  [ "$step" != "$last" ] && { echo "$(date +%T) 步骤 $step"; last=$step; }
  case "$st" in *'"phase":"busy"'*) sleep 1;; *) break;; esac
done
echo "$st" | grep -o '"frp_reuse":[a-z]*\|"ports":{[^}]*}\|"tunnel":{[^}]*}'
