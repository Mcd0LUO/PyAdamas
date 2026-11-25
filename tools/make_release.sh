#!/usr/bin/env bash
# tools/make_release.sh —— 打一个可迁移的发布包：bin/pyadamas + stdlib/ + 文档。
#
# 用法：bash tools/make_release.sh [构建目录]
#   默认构建目录 build/lead；产物写到 dist/pyadamas-<version>-linux-x86_64.tar.gz。
#
# 为什么带 stdlib：编译器运行时要按模块表加载 stdlib/ 下的 .py 与 sigs/*.pysig；
# 二进制里的编译期路径只对开发机有效。运行期解析顺序（src/driver/util.cpp）：
#   PYADAMAS_STDLIB 环境变量 → 编译期路径（存在时）→ <exe>/stdlib → <exe>/../stdlib
#   → <exe>/../share/pyadamas/stdlib。所以发布包用 bin/ + stdlib/ 的布局即可自洽。
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD=${1:-build/lead}
PA="$BUILD/pyadamas"
[ -x "$PA" ] || { echo "找不到编译器：$PA（先构建）" >&2; exit 2; }

VER=$("$PA" deps --json /dev/stdin <<< 'x = 1' 2>/dev/null | grep -o '"version": "[^"]*"' | head -1 | cut -d'"' -f4)
[ -n "$VER" ] || { echo "取版本号失败" >&2; exit 2; }
NAME="pyadamas-$VER-linux-x86_64"
DIST=dist
STAGE=$(mktemp -d)
OUT="$DIST/$NAME.tar.gz"
mkdir -p "$DIST" "$STAGE/$NAME/bin"
cp "$PA" "$STAGE/$NAME/bin/pyadamas"
mkdir -p "$STAGE/$NAME/stdlib"
tar -C stdlib --exclude='__pycache__' -cf - . | tar -C "$STAGE/$NAME/stdlib" -xf -
cp README.md LICENSE "$STAGE/$NAME/" 2>/dev/null || true
cat > "$STAGE/$NAME/RELEASE.txt" <<EOF
pyadamas $VER (linux-x86_64)

这是一个自包含的编译器包：bin/pyadamas + stdlib/（运行时从可执行文件旁边找 stdlib）。

用法：
  ./bin/pyadamas run hello.py
  ./bin/pyadamas build -O2 -o hello hello.py && ./hello
  ./bin/pyadamas check example.py

依赖：x86-64 Linux、glibc（本包在 Debian 13 / glibc 2.41 上构建）、binutils 的 as/ld。
stdlib 定位顺序：PYADAMAS_STDLIB 环境变量 → 编译期路径 → <exe>/stdlib → <exe>/../stdlib
→ <exe>/../share/pyadamas/stdlib。
EOF
tar -C "$STAGE" -czf "$OUT" "$NAME"
( cd "$DIST" && sha256sum "$NAME.tar.gz" > "$NAME.tar.gz.sha256" )
# 自检：解包到临时目录、不设任何环境变量、换个 cwd 跑一个用到 stdlib 的程序。
T=$(mktemp -d)
tar -C "$T" -xf "$OUT"
cat > "$T/smoke.py" <<'PY'
import sys, os, re, json, time
print('smoke', re.subn('a', 'X', 'banana'), json.dumps({'k': 1}), time.localtime(0).tm_year)
PY
( cd /tmp && "$T/$NAME/bin/pyadamas" run "$T/smoke.py" > "$T/out" 2>&1 ) || { echo "发布包自检失败" >&2; cat "$T/out"; exit 1; }
echo "自检输出: $(cat "$T/out")"
rm -rf "$T" "$STAGE"
echo "产物: $OUT（$(du -h "$OUT" | cut -f1)）"
echo "校验: $OUT.sha256"