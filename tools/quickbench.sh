#!/bin/bash
# quickbench.sh —— 对 bench/realistic 里的用例做「最好 N 次」计时，用于单版本回归。
#
#   bash tools/quickbench.sh                       # 默认 queue_sim str_build
#   bash tools/quickbench.sh nbody matmul          # 指定用例
#   PYADAMAS_BIN=build/dev/pyadamas REP=9 bash tools/quickbench.sh
#
# pyadamas 二进制路径按脚本位置推导，换机器/换构建目录不用改。
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=${PYADAMAS_BIN:-$ROOT/build/release/pyadamas}
OUT=${OUT:-/tmp/pyadamas-quickbench}
REP=${REP:-5}
mkdir -p "$OUT"
CASES="${*:-queue_sim str_build}"

fail=0
for c in $CASES; do
    src=$ROOT/bench/realistic/$c.py
    [ -f "$src" ] || { echo "no such case: $c"; fail=1; continue; }
    if ! "$BIN" build -o "$OUT/$c" "$src" > "$OUT/$c.build" 2>&1; then
        echo "BUILD FAIL $c"; tail -3 "$OUT/$c.build"; fail=1; continue
    fi
done

for c in $CASES; do
    [ -x "$OUT/$c" ] || continue
    "$OUT/$c" > "$OUT/$c.out" 2>&1
    best=999999999
    for _ in $(seq "$REP"); do
        s=$(date +%s%N)
        "$OUT/$c" > /dev/null 2>&1
        e=$(date +%s%N)
        d=$(( (e - s) / 1000000 ))
        [ "$d" -lt "$best" ] && best=$d
    done
    printf "%-14s %6d ms  out=[%s]\n" "$c" "$best" "$(head -c 40 "$OUT/$c.out")"
done
exit $fail
