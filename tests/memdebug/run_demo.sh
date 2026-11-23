#!/usr/bin/env bash
# run_demo.sh —— 证明 PYADAMAS_CANARY / PYADAMAS_GUARD 真的能抓到堆越界写。
#
# 用法：bash tests/memdebug/run_demo.sh [pyadamas 可执行文件]
# 默认用 build/lead/pyadamas。三段分别验证：
#   1) 两个开关都关：越界写静默通过 —— 生成代码的盲区（不做断言，只展示）；
#   2) PYADAMAS_CANARY=1：释放对象时 canary 校验失败 → panic；
#   3) PYADAMAS_GUARD=1：越界写撞 PROT_NONE 页 → 立刻 SIGSEGV。
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PY=${1:-${PYADAMAS_BIN:-$ROOT/build/lead/pyadamas}}
DEMO="$ROOT/tests/memdebug/demo.py"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

if [ ! -x "$PY" ]; then
    echo "找不到 pyadamas 可执行文件：$PY" >&2
    exit 2
fi

# cimport 的 link "tests/memdebug/oob.c" 是相对仓库根目录解析的（与
# tests/run/*.cpya 同口径），所以固定从仓库根跑。
cd "$ROOT" || exit 2

echo "=== 1) 默认（两个开关都关）：越界写是盲区 ==="
if ! "$PY" build --rebuild -o "$WORK/demo" "$DEMO" >/dev/null 2>"$WORK/build.err"; then
    echo "BUILD FAILED"; cat "$WORK/build.err"; exit 2
fi
"$WORK/demo" >/dev/null 2>&1
echo "exit=$?（越界写没有被打断；具体是否崩坏取决于堆布局）"

echo
echo "=== 2) PYADAMAS_CANARY=1：释放时校验尾部 canary ==="
if ! PYADAMAS_CANARY=1 "$PY" build --rebuild -o "$WORK/demo_can" "$DEMO" \
        >/dev/null 2>"$WORK/build2.err"; then
    echo "BUILD FAILED"; cat "$WORK/build2.err"; exit 2
fi
out=$(PYADAMAS_CANARY=1 "$WORK/demo_can" 2>&1)
rc=$?
printf '%s\n' "$out"
echo "exit=$rc"
case "$out" in
    *"canary 被踩"*) echo "CANARY OK：抓到越界写";;
    *) echo "CANARY FAIL：期望 stderr 出现「canary 被踩」"; exit 1;;
esac

echo
echo "=== 3) PYADAMAS_GUARD=1：对象尾部紧跟 PROT_NONE 页 ==="
if ! PYADAMAS_GUARD=1 "$PY" build --rebuild -o "$WORK/demo_grd" "$DEMO" \
        >/dev/null 2>"$WORK/build3.err"; then
    echo "BUILD FAILED"; cat "$WORK/build3.err"; exit 2
fi
PYADAMAS_GUARD=1 "$WORK/demo_grd" >/dev/null 2>&1
rc=$?
echo "exit=$rc（139 = SIGSEGV，越界写当场触发）"
if [ "$rc" -eq 139 ]; then
    echo "GUARD OK：抓到越界写"
else
    echo "GUARD FAIL：期望 exit=139"; exit 1
fi
