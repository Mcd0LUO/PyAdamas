#!/usr/bin/env bash
# py23 测试套件：正例 / 反例 / 运行 / 未建模 / 多模块 + 缓存。
# 用法：run_tests.sh <build-dir> <py23-可执行文件>
#
# 敏捷模式：
#   PY23_TEST_FILTER='3[0-4]*' PY23_TEST_JOBS=8 bash tests/run_tests.sh build/dev build/dev/py23
#   PY23_TEST_FILTER 按 basename 匹配（shell glob），只跑选中的用例。
#   PY23_TEST_JOBS 控制并行度，默认 nproc。
set -u

BUILD=${1:?build dir}
PY23=${2:?py23 binary}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1

JOBS=${PY23_TEST_JOBS:-$(nproc 2>/dev/null || echo 4)}
FILTER=${PY23_TEST_FILTER:-}
match() {
    [ -z "$FILTER" ] && return 0
    case "$(basename "$1")" in
        $FILTER) return 0 ;;
        *) return 1 ;;
    esac
}

fail=0
prog="$BUILD/prog.tmp"
out="$BUILD/out.txt"
exp="$BUILD/exp.txt"

for f in tests/*.py; do
    match "$f" || continue
    name=$(basename "$f")
    printf '  %-28s ' "$name"
    if "$PY23" check "$f" > "$BUILD/check.txt" 2>&1; then
        head -1 "$BUILD/check.txt"
    else
        printf 'FAILED\n'
        sed -n '1,6p' "$BUILD/check.txt"
        fail=1
    fi
done

for f in tests/fail/*.py; do
    match "$f" || continue
    name=$(basename "$f")
    want=$(head -1 "$f" | sed -n 's/^# expect: //p')
    got=$("$PY23" check "$f" 2>&1 >/dev/null | head -1)
    printf '  %-28s ' "$name"
    if [ -z "$got" ]; then
        printf 'MISSED (expected: %s)\n' "$want"
        fail=1
    elif ! printf '%s' "$got" | grep -qF "$want"; then
        printf 'WRONG: %s\n' "$got"
        fail=1
    else
        printf 'rejected (%s)\n' "$want"
    fi
done

# ── 运行用例：并行编译 + 运行，结果按文件名顺序汇总 ──────────────
logdir="$BUILD/testlogs.$$"
rm -rf "$logdir"
mkdir -p "$logdir"

run_test() {
    local f=$1 name res
    name=$(basename "$f")
    res="$logdir/$name.res"
    local p="$logdir/$name.prog" o="$logdir/$name.out" e="$logdir/$name.exp"
    if ! "$PY23" run --rebuild -o "$p" "$f" > "$o" 2>&1; then
        { echo FAILED; sed -n '1,6p' "$o"; } > "$res"
        return
    fi
    sed -n '/^# --- expect ---$/,$p' "$f" | tail -n +2 | sed -E 's/^# ?//' > "$e"
    if diff -q "$e" "$o" > /dev/null 2>&1; then
        echo "ran ok" > "$res"
    else
        { echo "OUTPUT DIFF"; diff "$e" "$o" | head -12; } > "$res"
    fi
}

for f in tests/run/*.py; do
    match "$f" || continue
    ( run_test "$f" ) &
    while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do
        wait -n || true
    done
done
wait

for f in tests/run/*.py; do
    match "$f" || continue
    name=$(basename "$f")
    res="$logdir/$name.res"
    printf '  %-28s ' "$name"
    if [ -f "$res" ]; then
        first=$(head -1 "$res")
        printf '%s\n' "$first"
        tail -n +2 "$res"
        [ "$first" = "ran ok" ] || fail=1
    else
        printf 'MISSING RESULT\n'
        fail=1
    fi
done

for f in tests/unsupported/*.py; do
    match "$f" || continue
    name=$(basename "$f")
    want=$(head -1 "$f" | sed -n 's/^# unsupported: //p')
    got=$("$PY23" check -v "$f" 2>&1 >/dev/null | head -1)
    printf '  %-28s ' "$name"
    if ! printf '%s' "$got" | grep -qF "$want"; then
        printf 'NOT MARKED (expected: %s)\n' "$want"
        fail=1
    elif "$PY23" build -o "$prog" "$f" > /dev/null 2>&1; then
        printf 'COMPILED ANYWAY\n'
        fail=1
    else
        printf 'parsed, refused by backend\n'
    fi
done

# ── 多模块：并行（各自独立目录，缓存互不影响）──────────────────
modlog="$BUILD/modlogs.$$"
rm -rf "$modlog"
mkdir -p "$modlog"

run_module() {
    local d=$1 name res
    name=$(basename "$d")
    res="$modlog/$name.res"
    local p="$modlog/$name.prog" o="$modlog/$name.out" e="$modlog/$name.exp" err="$modlog/$name.err"
    if ! "$PY23" run --rebuild -o "$p" "$d/main.py" > "$o" 2>&1; then
        { echo FAILED; sed -n '1,8p' "$o"; } > "$res"
        return
    fi
    sed -n '/^# --- expect ---$/,$p' "$d/main.py" | tail -n +2 | sed -E 's/^# ?//' > "$e"
    if ! diff -q "$e" "$o" > /dev/null 2>&1; then
        { echo "OUTPUT DIFF"; diff "$e" "$o" | head -12; } > "$res"
        return
    fi
    if ! "$PY23" run -v -o "$p" "$d/main.py" > /dev/null 2> "$err"; then
        echo "SECOND RUN FAILED" > "$res"
        return
    fi
    if ! grep -q "cached module" "$err"; then
        echo "CACHE MISS" > "$res"
        return
    fi
    echo "ran ok" > "$res"
}

for d in tests/modules/*/; do
    match "${d}main.py" || continue
    ( run_module "$d" ) &
    while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do
        wait -n || true
    done
done
wait

for d in tests/modules/*/; do
    match "${d}main.py" || continue
    name=$(basename "$d")
    res="$modlog/$name.res"
    printf '  %-28s ' "module:$name"
    if [ -f "$res" ]; then
        first=$(head -1 "$res")
        printf '%s\n' "$first"
        tail -n +2 "$res"
        [ "$first" = "ran ok" ] || fail=1
    else
        printf 'MISSING RESULT\n'
        fail=1
    fi
done

rm -rf "$logdir" "$modlog"
rm -f "$prog" "$out" "$exp" "$BUILD/check.txt"
exit "$fail"
