#!/usr/bin/env bash
# traceback 行号（0.494）：未捕获异常的回溯必须给出**真实行号**（以前是 line 0）。
#
# 为什么不是 tests/run 用例：回溯打在 stderr 上，而 tests/run 的 expect 只比 stdout。
# 这里单独跑一个固定行号的小程序，断言 stderr 里出现 line 8 / line 17 与函数名。
#
# 已知边界（不在此断言）：-O2 会把小函数内联，物理栈只剩一帧；帧数对 -O0 成立。
set -u
BUILD_DIR=${1:?usage: traceback_lines.sh <build_dir> <pyadamas>}
PA=${2:?usage: traceback_lines.sh <build_dir> <pyadamas>}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export PYADAMAS_CACHE_DIR="$work/cache"
mkdir -p "$work/cache"

cat > "$work/tb.py" <<'PYEOF'
def inner(n: int) -> None:
    acc: int = 0
    i: int = 0
    while i < n:
        acc = acc + i * 3
        i = i + 1
    if acc > 0:
        raise ValueError("boom")

def outer(n: int) -> None:
    acc: int = 0
    i: int = 0
    while i < n:
        acc = acc + i
        i = i + 1
    if acc >= 0:
        inner(n)

outer(4)
PYEOF

if ! "$PA" build --rebuild -o "$work/tb" "$work/tb.py" >"$work/build.log" 2>&1; then
    echo "build failed:"; sed -n '1,6p' "$work/build.log"; exit 1
fi

err=$("$work/tb" 2>&1 >/dev/null)
echo "$err" | sed 's/^/  /'

fail=0
if ! printf '%s' "$err" | grep -q 'Traceback (most recent call last):'; then
    echo "  MISSING traceback header"; fail=1
fi
# raise 在 inner 的第 8 行（1-based），内联后函数名可能是 __pa_main，但行号必须是 8。
if ! printf '%s' "$err" | grep -q 'line 8,'; then
    echo "  MISSING 'line 8'（抛出点真实行号）"; fail=1
fi
if printf '%s' "$err" | grep -q 'line 0,'; then
    echo "  STILL 'line 0'（行号归因没生效）"; fail=1
fi
if ! printf '%s' "$err" | grep -q 'ValueError: boom'; then
    echo "  MISSING uncaught message"; fail=1
fi

if [ $fail -ne 0 ]; then echo "traceback 行号回归失败"; exit 1; fi
echo "traceback 行号 4/4 通过"