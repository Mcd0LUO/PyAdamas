#!/usr/bin/env bash
# 保守 GC 模式等价性回归（3.10）：PYADAMAS_GC_CONSERVATIVE=1 必须与默认模式
# 输出逐字节相同，且不能崩（UAF/碎片化都得是 0）。
#
# 为什么单独一条：run_tests.sh 不传这个 env，保守模式曾经静默红（161 段错误，
# 根因是 float 盒缓存只被静态数组持有、没进 GC 根表，见 docs/memory-debug.md §9）。
#
# 用法: gc_conservative.sh <build_dir> <pyadamas>
set -u
BUILD_DIR=${1:?usage: gc_conservative.sh <build_dir> <pyadamas>}
PA=${2:?usage: gc_conservative.sh <build_dir> <pyadamas>}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export PYADAMAS_CACHE_DIR="$work/cache"
mkdir -p "$work/cache"

src=tests/run/161_gc_conservative_equiv.py
prog="$work/161.prog"
if ! "$PA" build -O1 -o "$prog" "$src" > "$work/build.log" 2>&1; then
    echo "  build 161: FAILED"
    sed -n '1,6p' "$work/build.log"
    exit 1
fi

sed -n '/^# --- expect ---$/,$p' "$src" | tail -n +2 | sed -E 's/^# ?//' > "$work/expect.txt"

fail=0
"$prog" > "$work/default.out" 2>&1
if [ $? -ne 0 ]; then
    echo "  default mode: FAILED (non-zero exit)"
    fail=1
fi
PYADAMAS_GC_CONSERVATIVE=1 "$prog" > "$work/cons.out" 2>&1
rc=$?
if [ $rc -ne 0 ]; then
    echo "  conservative mode: FAILED (rc=$rc)"
    sed -n '1,6p' "$work/cons.out"
    fail=1
fi
if ! diff -q "$work/expect.txt" "$work/default.out" > /dev/null 2>&1; then
    echo "  default mode: OUTPUT DIFF"
    diff "$work/expect.txt" "$work/default.out" | head -6
    fail=1
fi
if ! diff -q "$work/default.out" "$work/cons.out" > /dev/null 2>&1; then
    echo "  conservative mode: output differs from default"
    diff "$work/default.out" "$work/cons.out" | head -6
    fail=1
fi

if [ $fail -ne 0 ]; then
    echo "保守 GC 模式等价性回归失败"
    exit 1
fi
echo "保守 GC 模式等价性：默认 / PYADAMAS_GC_CONSERVATIVE=1 输出一致（161）"
