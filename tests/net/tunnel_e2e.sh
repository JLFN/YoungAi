#!/usr/bin/env bash
# tunnel_e2e.sh — 设置弹窗的穿透整条链端到端(src/train/train_net.c 一族): 真 ssh 部署 frps → 本机 frpc → 公网口 → 上传证书变 https。
# 要一台"云服务器": 有 sshd(开密码登录)和 systemd 的 Linux。没有真云服务器时用 OrbStack 起一台(Mac 上两分钟):
#   orb create ubuntu:noble ds4cloud
#   orb -m ds4cloud -u root bash -c 'apt-get update && apt-get install -y openssh-server sudo; printf "PasswordAuthentication yes\nPermitRootLogin yes\n" > /etc/ssh/sshd_config.d/99-test.conf; echo root:<密码> | chpasswd; systemctl restart ssh'
# 用法: tests/net/tunnel_e2e.sh <服务器IP> <用户> <密码> [页面端口 18000]
# 会动的东西: 本机 ~/.ds4/studio/(登录、cloud、frp/、tls/ —— 起跑前若已存在就整个挪到 ~/.ds4/studio.bak.<时间> 保全, 跑完不自动还原, 看最后一行提示);
#   服务器上装 /usr/local/bin/ds4-frps + ds4-frps.service(和正式用一样)。
set -u
HOST=$1 USER_=$2 PASS=$3 PORT=${4:-18000}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SD=$HOME/.ds4/studio
LOG=/tmp/ds4-tunnel-e2e.log
c() { curl -s --noproxy "*" -m 20 "$@"; }   # 本机有 http_proxy 时 curl 会把内网地址也送去代理
fail=0
ok()  { echo "PASS $*"; }
bad() { echo "FAIL $*"; fail=$((fail+1)); }

[ -x "$ROOT/ds4-train" ] || { echo "先 make ds4-train"; exit 2; }
if [ -e "$SD" ]; then BK=$SD.bak.$(date +%s); mv "$SD" "$BK"; echo "已把原 $SD 挪到 $BK"; fi

"$ROOT/ds4-train" --port "$PORT" --root "$ROOT" > "$LOG" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null' EXIT
for _ in $(seq 50); do c -o /dev/null "http://127.0.0.1:$PORT/" && break; sleep 0.2; done

api() {   # api <方法> <路径> [JSON 体]
  if [ "$1" = GET ]; then c -H "X-DS4-Session: $SESS" "http://127.0.0.1:$PORT$2"
  else c -X POST -H "X-DS4-Session: $SESS" -H 'Content-Type: application/json' -H 'X-DS4-Studio: 1' --data "${3:-{\}}" "http://127.0.0.1:$PORT$2"; fi
}
jget() { sed -n "s/.*\"$1\":\"\\([^\"]*\\)\".*/\\1/p" | head -1; }
wait_tunnel() {   # 等穿透走完, 打印最终 phase
  local st
  for _ in $(seq 240); do st=$(api GET /api/net/status); case "$st" in *'"phase":"busy"'*) sleep 0.5;; *) break;; esac; done
  echo "$st"
}

SESS=$(c -X POST -H 'X-DS4-Studio: 1' -H 'Content-Type: application/json' --data '{"user":"e2euser","password":"e2e-pass-123"}' "http://127.0.0.1:$PORT/api/auth/setup" | jget session)
[ -n "$SESS" ] && ok "登录设置" || { bad "登录设置"; exit 1; }

# 1) 地址校验: 以 - 开头的主机会被 ssh 当成选项, 必须拒
r=$(api POST /api/net/save '{"host":"-oProxyCommand=touch /tmp/pwned","port":"22","user":"root","password":"x"}')
case "$r" in *bad_host*) ok "拒 - 开头的地址";; *) bad "拒 - 开头的地址: $r";; esac

# 2) 错密码 → ssh_auth
api POST /api/net/save "{\"host\":\"$HOST\",\"port\":\"22\",\"user\":\"$USER_\",\"password\":\"wrong-$PASS\"}" > /dev/null
api POST /api/net/tunnel '{"on":true}' > /dev/null
st=$(wait_tunnel)
case "$st" in *'"error":"ssh_auth'*) ok "错密码报 ssh_auth";; *) bad "错密码: $st";; esac

# 3) 对的密码 → 部署 → http 公网口
r=$(api POST /api/net/save "{\"host\":\"$HOST\",\"port\":\"22\",\"user\":\"$USER_\",\"password\":\"$PASS\"}")
case "$r" in *"$PASS"*) bad "状态里回传了密码";; *) ok "状态不回传密码";; esac
st=$(wait_tunnel)
case "$st" in *'"phase":"up"'*) ok "部署 + frpc 连通(http)";; *) bad "部署: $st";; esac
echo "  方案说明: $(echo "$st" | sed -n "s/.*\"note\":\"\([^\"]*\)\".*/\1/p")"
code=$(c -o /dev/null -w '%{http_code}' "http://$HOST:$PORT/")
[ "$code" = 200 ] && ok "从服务器公网口打开页面 http://$HOST:$PORT/" || bad "公网口页面 HTTP $code"
code=$(c -o /dev/null -w '%{http_code}' "http://$HOST:$PORT/api/train/status")
[ "$code" = 401 ] && ok "公网进来的数据接口要登录(401)" || bad "公网数据接口没拦: HTTP $code"

# 4) 证书: 自签 CA → 域名证书, 打成"腾讯云式"压缩包(Nginx/ 只有叶证书, 根目录 bundle 带链, 外加一份不配对的私钥)
T=$(mktemp -d); DOM=e2e.ds4.test
( cd "$T" && openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.crt -days 3 -subj /CN=e2e-ca 2>/dev/null \
  && openssl req -newkey rsa:2048 -nodes -keyout "$DOM.key" -out x.csr -subj "/CN=$DOM" 2>/dev/null \
  && printf "subjectAltName=DNS:%s\n" "$DOM" > ext && openssl x509 -req -in x.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out leaf.crt -days 2 -extfile ext 2>/dev/null \
  && mkdir -p pkg/Nginx pkg/other && cat leaf.crt ca.crt > "pkg/${DOM}_bundle.crt" && cp "$DOM.key" "pkg/$DOM.key" && cp leaf.crt "pkg/Nginx/$DOM.crt" \
  && openssl genrsa -out pkg/other/old.key 2048 2>/dev/null && (cd pkg && zip -qr ../cert.zip .) && (mkdir bad && cd bad && cp ../pkg/other/old.key x.key && cp ../leaf.crt x.crt && zip -q ../bad.zip x.key x.crt) )
r=$(c -X POST -H "X-DS4-Session: $SESS" -H 'Content-Type: application/zip' --data-binary @"$T/bad.zip" "http://127.0.0.1:$PORT/api/net/cert")
case "$r" in *mismatch*) ok "证书私钥不配对被拒";; *) bad "不配对: $r";; esac
r=$(c -X POST -H "X-DS4-Session: $SESS" -H 'Content-Type: application/zip' --data-binary @"$T/cert.zip" "http://127.0.0.1:$PORT/api/net/cert")
case "$r" in *'"chain":2'*) ok "证书装上, 挑中带链的 bundle";; *) bad "证书上传: $r";; esac
st=$(wait_tunnel)
case "$st" in *'"phase":"up"'*'"tls":true'*|*'"tls":true'*'"phase":"up"'*) ok "换证书后 frpc 重起成 https";; *) bad "https 重起: $st";; esac
code=$(c --cacert "$T/ca.crt" --resolve "$DOM:443:$HOST" -o /dev/null -w '%{http_code}' "https://$DOM/")
[ "$code" = 200 ] && ok "https://$DOM/ 证书校验通过并打开页面" || bad "https 页面 HTTP $code"
case "$st" in *dns_none*|*dns_mismatch*) ok "域名没指向服务器给了提醒";; *) bad "缺域名解析提醒: $st";; esac   # 开着 Clash 假 IP 时测试域名会解析成 198.18.x

# 4b) 公网端口设成 8443: 重新部署(allowPorts/防火墙跟着改), 公网地址带 :8443, 443 不再通
r=$(api POST /api/net/save "{\"host\":\"$HOST\",\"port\":\"22\",\"user\":\"$USER_\",\"password\":\"\",\"pub\":\"8443\"}")
st=$(wait_tunnel)
case "$st" in *"\"url\":\"https://$DOM:8443/\""*'"phase":"up"'*|*'"phase":"up"'*"\"url\":\"https://$DOM:8443/\""*) ok "公网端口 8443 重新部署, 地址带端口";; *) bad "8443: $st";; esac
code=$(c --cacert "$T/ca.crt" --resolve "$DOM:8443:$HOST" -o /dev/null -w '%{http_code}' "https://$DOM:8443/")
[ "$code" = 200 ] && ok "https://$DOM:8443/ 打开页面" || bad "8443 页面 HTTP $code"
code=$(c --cacert "$T/ca.crt" --resolve "$DOM:443:$HOST" -o /dev/null -w '%{http_code}' "https://$DOM/")
[ "$code" != 200 ] && ok "443 不再转发" || bad "443 还通"
api POST /api/net/save "{\"host\":\"$HOST\",\"port\":\"22\",\"user\":\"$USER_\",\"password\":\"\",\"pub\":\"\"}" > /dev/null
st=$(wait_tunnel)
case "$st" in *"\"url\":\"https://$DOM/\""*) ok "清空公网端口回到 443";; *) bad "回 443: $st";; esac

# 5) 重启主进程: 穿透开着就自己把 frpc 拉起来(不再走 ssh)
kill $PID; wait $PID 2>/dev/null
"$ROOT/ds4-train" --port "$PORT" --root "$ROOT" >> "$LOG" 2>&1 &
PID=$!
for _ in $(seq 50); do c -o /dev/null "http://127.0.0.1:$PORT/" && break; sleep 0.2; done
st=$(wait_tunnel)
case "$st" in *'"deploy":false'*'"phase":"up"'*|*'"phase":"up"'*'"deploy":false'*) ok "重启后自动只起 frpc";; *) bad "重启自启: $st";; esac
n=$(pgrep -f "$SD/frp/frpc.toml" | wc -l | tr -d ' ')   # 只数我们的: 机器上可能有别的项目自己的 frpc
[ "$n" = 1 ] && ok "只有一个 frpc(旧的被按 pid 文件收掉)" || bad "frpc 个数 $n"

# 5b) SSH 控制通道: 控制口不对外; 杀掉 ssh 自己重连; 只有一条
fp=$(echo "$st" | sed -n 's/.*"ports":{"frp":\([0-9]*\).*/\1/p')
case "$st" in
  *'"frp_reuse":true'*) echo "SKIP 控制口检查: 复用的是服务器上别人的 frps, 它对不对外由它自己的配置决定";;
  *) c -m 4 -o /dev/null "http://$HOST:$fp/" ; rc=$?; [ "$rc" = 7 ] && ok "frps 控制口 $fp 从外面连不上(只听服务器本机)" || bad "控制口 $fp 对外可连(curl rc $rc)";;
esac
n=$(pgrep -f "ssh -N .*-L 127.0.0.1:" | wc -l | tr -d ' ')
[ "$n" = 1 ] && ok "只有一条 SSH 通道" || bad "SSH 通道条数 $n"
pkill -f "ssh -N .*-L 127.0.0.1:"
code=000; for _ in $(seq 40); do sleep 1; code=$(c --cacert "$T/ca.crt" --resolve "$DOM:443:$HOST" -o /dev/null -w '%{http_code}' "https://$DOM/"); [ "$code" = 200 ] && break; done
[ "$code" = 200 ] && ok "杀掉 SSH 通道后自己重连, 页面恢复" || bad "SSH 断后没恢复: HTTP $code"

# 6) 关穿透: frpc 没了, 公网口不通
api POST /api/net/tunnel '{"on":false}' > /dev/null; sleep 1
pgrep -f "$SD/frp/frpc.toml" > /dev/null && bad "关了穿透 frpc 还在" || ok "关穿透 frpc 退出"
pgrep -f "ssh -N .*-L 127.0.0.1:" > /dev/null && bad "关了穿透 SSH 通道还在" || ok "关穿透 SSH 通道退出"
code=$(c --cacert "$T/ca.crt" --resolve "$DOM:443:$HOST" -o /dev/null -w '%{http_code}' "https://$DOM/")
[ "$code" != 200 ] && ok "关了以后公网打不开" || bad "关了公网还能开"

rm -rf "$T"
echo "== $fail 项失败(日志 $LOG; 本次产生的 $SD 留着可看${BK:+, 原目录在 $BK})"
exit $fail
