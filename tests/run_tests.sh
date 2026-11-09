#!/usr/bin/env bash
# py23 测试套件：正例 / 反例 / 运行 / 未建模 / 多模块 + 缓存。
# 用法：run_tests.sh <build-dir> <py23-可执行文件>
set -u

BUILD=${1:?build dir}
PY23=${2:?py23 binary}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1

fail=0
prog="$BUILD/prog.tmp"
out="$BUILD/out.txt"
exp="$BUILD/exp.txt"

for f in tests/*.py; do
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

for f in tests/run/*.py; do
    name=$(basename "$f")
    printf '  %-28s ' "$name"
    if ! "$PY23" run --rebuild -o "$prog" "$f" > "$out" 2>&1; then
        printf 'FAILED\n'
        sed -n '1,6p' "$out"
        fail=1
        continue
    fi
    sed -n '/^# --- expect ---$/,$p' "$f" | tail -n +2 | sed -E 's/^# ?//' > "$exp"
    if diff -q "$exp" "$out" > /dev/null 2>&1; then
        printf 'ran ok\n'
    else
        printf 'OUTPUT DIFF\n'
        diff "$exp" "$out" | head -12
        fail=1
    fi
done

for f in tests/unsupported/*.py; do
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

modprog="$BUILD/modprog.tmp"
modout="$BUILD/modout.txt"
modexp="$BUILD/modexp.txt"
moderr="$BUILD/moderr.txt"
for d in tests/modules/*/; do
    name=$(basename "$d")
    printf '  %-28s ' "module:$name"
    if ! "$PY23" run --rebuild -o "$modprog" "$d/main.py" > "$modout" 2>&1; then
        printf 'FAILED\n'
        sed -n '1,8p' "$modout"
        fail=1
        continue
    fi
    sed -n '/^# --- expect ---$/,$p' "$d/main.py" | tail -n +2 | sed -E 's/^# ?//' > "$modexp"
    if ! diff -q "$modexp" "$modout" > /dev/null 2>&1; then
        printf 'OUTPUT DIFF\n'
        diff "$modexp" "$modout" | head -12
        fail=1
        continue
    fi
    if ! "$PY23" run -v -o "$modprog" "$d/main.py" > /dev/null 2> "$moderr"; then
        printf 'SECOND RUN FAILED\n'
        fail=1
        continue
    fi
    if ! grep -q "cached module" "$moderr"; then
        printf 'CACHE MISS\n'
        fail=1
        continue
    fi
    printf 'ran ok\n'
done

rm -f "$prog" "$out" "$exp" "$BUILD/check.txt" \
      "$modprog" "$modout" "$modexp" "$moderr"
exit "$fail"
