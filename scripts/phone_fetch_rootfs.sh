#!/bin/bash
# phone_fetch_rootfs.sh — 把云手机的 Android 根文件系统(redroid 镜像的那一层)从 Docker Hub 拉到 gguf/phone/, 不装 docker(10-13)。
#
# 怎么用:  scripts/phone_fetch_rootfs.sh            # 在能上 Docker Hub 的机器上跑(spark 直连/经 mihomo 都到不了 registry, 10-13 实测; Mac 可以)
#          scripts/phone_fetch_rootfs.sh push       # 拉完顺手 scp 到 spark:~/ds4-main/gguf/phone/
# 产物:    gguf/phone/layer.tar.gz   (镜像唯一的一层, 640 MB gzip; sha256 与清单里的 digest 对过才落盘)
#          gguf/phone/image.json     (镜像配置: 入口是 "/init qemu=1 androidboot.hardware=redroid", 运行时照它拼参数)
#          gguf/phone/manifest.json  (arm64 清单, 留作溯源)
# 之后:    spark 上 sudo scripts/phone_prepare_linux.sh 把这一层解进 gguf/phone/rootfs/(要 root: 文件属主/能力 xattr 必须原样)。
# 为什么走 registry 协议而不 docker pull: 用户 10-13 定"安装 docker 我就不允许"; 一层 tar 本来就是 registry 上的一个 blob, curl 就够。
# 出错会怎样: 没网/被墙 → 第一步拿 token 就卡住(默认 60 s 超时报错); sha256 对不上 → 删掉重拉, 不留半截文件。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/gguf/phone"
IMAGE="redroid/redroid"
TAG="13.0.0_64only-latest"   # 13: 新内核没有 ashmem(5.18 起删), 镜像要 ≥12 才能靠 memfd; 64only 省一份 32 位 zygote
ARCH="arm64"                  # spark = GB10 aarch64
REG="https://registry-1.docker.io/v2/$IMAGE"
mkdir -p "$OUT" || exit 1

tok() { curl -sf -m 60 "https://auth.docker.io/token?service=registry.docker.io&scope=repository:$IMAGE:pull" | jq -r .token; }
TOK="$(tok)"; [ -n "$TOK" ] && [ "$TOK" != null ] || { echo "拿不到 Docker Hub 的 pull token(网不通?)"; exit 1; }
H="Authorization: Bearer $TOK"

# 多架构索引 → 本架构的清单 → 配置 + 层
IDX="$(curl -sf -m 60 -H "$H" -H "Accept: application/vnd.oci.image.index.v1+json, application/vnd.docker.distribution.manifest.list.v2+json" "$REG/manifests/$TAG")" || { echo "拿不到索引"; exit 1; }
MDIG="$(echo "$IDX" | jq -r --arg a "$ARCH" '.manifests[] | select(.platform.architecture==$a and .platform.os=="linux") | .digest')"
[ -n "$MDIG" ] || { echo "索引里没有 $ARCH"; exit 1; }
curl -sf -m 60 -H "$H" -H "Accept: application/vnd.oci.image.manifest.v1+json, application/vnd.docker.distribution.manifest.v2+json" "$REG/manifests/$MDIG" > "$OUT/manifest.json" || { echo "拿不到清单"; exit 1; }
NL="$(jq '.layers | length' "$OUT/manifest.json")"
[ "$NL" = 1 ] || { echo "镜像有 $NL 层, 本脚本只处理 1 层的 redroid(多层要按序叠加+处理 whiteout, 先别拉)"; exit 1; }
CDIG="$(jq -r .config.digest "$OUT/manifest.json")"
LDIG="$(jq -r '.layers[0].digest' "$OUT/manifest.json")"
LSZ="$(jq -r '.layers[0].size' "$OUT/manifest.json")"
curl -sfL -m 60 -H "$H" "$REG/blobs/$CDIG" > "$OUT/image.json" || { echo "拿不到镜像配置"; exit 1; }
echo "入口: $(jq -c .config.Entrypoint "$OUT/image.json")"

# 层: 已有且 sha256 对得上就不重拉
want="${LDIG#sha256:}"
have() { [ -f "$OUT/layer.tar.gz" ] && [ "$(shasum -a 256 "$OUT/layer.tar.gz" 2>/dev/null | cut -d' ' -f1)" = "$want" ]; }
if have; then echo "layer.tar.gz 已在且校验通过"; else
    echo "拉层 $LDIG ($((LSZ / 1048576)) MB) …"
    curl -fL -m 3600 -H "$H" --progress-bar "$REG/blobs/$LDIG" -o "$OUT/layer.tar.gz.part" || { rm -f "$OUT/layer.tar.gz.part"; echo "下载失败"; exit 1; }
    mv "$OUT/layer.tar.gz.part" "$OUT/layer.tar.gz"
    have || { rm -f "$OUT/layer.tar.gz"; echo "sha256 对不上, 已删, 重跑"; exit 1; }
    echo "校验通过"
fi
echo "$LDIG" > "$OUT/layer.sha256"
ls -l "$OUT"

if [ "${1:-}" = push ]; then
    ssh spark 'mkdir -p ~/ds4-main/gguf/phone' && scp "$OUT/layer.tar.gz" "$OUT/layer.sha256" "$OUT/image.json" "$OUT/manifest.json" spark:ds4-main/gguf/phone/ && echo "已传到 spark:~/ds4-main/gguf/phone/"
fi
