#!/bin/sh
# 把项目网站拼成可发布的目录 website/_site (GitHub Pages 发的就是它; 本机预览也用它)。
#   用法: sh website/build.sh            → website/_site/
#        sh website/build.sh /tmp/out    → 指定输出目录
#   预览: python3 -m http.server -d website/_site 8080   然后开 http://localhost:8080/
# 截图和 logo 不在 website/ 里另存一份: 它们的正本在 docs/img/ 和 web/, README 也引用同一批文件,
# 在这里拷贝而不是复制进仓库, 截图更新后网站自动跟上。缺了哪张就直接报错退出, 不发一个缺图的站。
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
out=${1:-"$here/_site"}

rm -rf "$out"
mkdir -p "$out/img"
cp "$here/index.html" "$here/guide.html" "$out/"
cp -R "$here/assets" "$out/assets"

for f in studio-agent.png studio-agent-en.png arch.png arch-en.png; do
  cp "$root/docs/img/$f" "$out/img/$f"
done
cp "$root/web/logo.jpg" "$out/img/logo.jpg"

# GitHub Pages 默认过 Jekyll, 会吞掉下划线开头的文件; 我们是纯静态页, 关掉它
touch "$out/.nojekyll"
echo "website built: $out"
