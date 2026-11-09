#!/usr/bin/env bash
# 优化等级回归：同一批用例分别用 -O0 / -O1 / -O2 编译运行并比对输出。
# 用法：opt_levels.sh <build-dir> <py23>
set -u

BUILD=${1:?build dir}
PY23=${2:?py23 binary}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1

FILES=(
    tests/run/04_containers.py
    tests/run/19_comprehension.py
    tests/run/44_bounds_elim.py
    tests/run/45_opt_semantics.py
)

fail=0
for lvl in 0 1 2; do
    for f in "${FILES[@]}"; do
        name=$(basename "$f" .py)
        printf '  O%s %-32s ' "$lvl" "$name"
        prog="$BUILD/opt_${lvl}_${name}.prog"
        out="$BUILD/opt_${lvl}_${name}.out"
        exp="$BUILD/opt_${lvl}_${name}.exp"
        if ! "$PY23" run --rebuild "-O$lvl" -o "$prog" "$f" > "$out" 2>&1; then
            printf 'FAILED\n'
            sed -n '1,6p' "$out"
            fail=1
            continue
        fi
        sed -n '/^# --- expect ---$/,$p' "$f" | tail -n +2 | sed -E 's/^# ?//' > "$exp"
        if diff -q "$exp" "$out" > /dev/null 2>&1; then
            printf 'ok\n'
        else
            printf 'OUTPUT DIFF\n'
            diff "$exp" "$out" | head -12
            fail=1
        fi
    done
done
exit "$fail"
