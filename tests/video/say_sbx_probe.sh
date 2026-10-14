#!/bin/bash
# say_sbx_probe.sh — 探针(不进门): macOS 的 say 在助理沙盒(Seatbelt deny default + exec 模式)里出 wav 还缺哪些放行。
# 规则照 src/assistant/sbx_mac.c 的 exec 模式拼, 再按参数追加: 每个参数是一行 SBPL。
# 用法: tests/video/say_sbx_probe.sh '(allow mach-lookup (global-name "com.apple.xxx"))' …
# 打印 say 的退出码和产物大小; 退出码非 0 时用 `log show --last 1m --predicate 'sender=="Sandbox"'` 看被拒的是什么。
set -u
D=$(mktemp -d /tmp/saysbx.XXXXXX); D=$(cd "$D" && pwd -P)
P="(version 1)
(deny default)
(allow file-read* file-write* (subpath \"$D\"))
(allow process-fork)
(allow process-exec)
(allow signal (target same-sandbox))
(allow sysctl-read)
(allow file-read* (subpath \"/usr\") (subpath \"/bin\") (subpath \"/sbin\") (subpath \"/System\") (subpath \"/Library/Apple\") (subpath \"/private/etc\") (subpath \"/private/var/db/timezone\") (subpath \"/private/var/db/dyld\") (literal \"/dev/urandom\") (literal \"/dev/random\") (literal \"/dev/zero\"))
(allow file-read* file-write-data (literal \"/dev/null\"))
(allow file-read* (literal \"/\"))
(allow file-read-metadata (literal \"/\") (literal \"/private\") (literal \"/private/var\") (literal \"/var\") (literal \"/etc\") (literal \"/tmp\") (literal \"/Library\") (literal \"/private/tmp\"))"
for x in "$@"; do P="$P
$x"; done
sandbox-exec -p "$P" /usr/bin/say -v "${VOICE:-Lili}" -o "$D/out.wav" --data-format=LEI16@22050 "${TEXT:-你好，这是沙盒里的语音测试。}"
echo "say exit=$? size=$(stat -f %z "$D/out.wav" 2>/dev/null || echo none)"
rm -rf "$D"
