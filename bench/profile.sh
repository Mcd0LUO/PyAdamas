#!/usr/bin/env bash
# 用 perf 采样一个 bench 用例，打印热点函数（self time）和主要调用链。
#
# 用法：
#   bench/profile.sh 40_map              # 用例名（bench/builtins/40_map.py）
#   bench/profile.sh path/to/case.py     # 或直接给文件
#   PERF_FREQ=1999 bench/profile.sh 47_open
#
# 环境变量：
#   PYADAMAS_BIN      编译器路径（默认自动找 build/lead/pyadamas、/w/build/dev/pyadamas）
#   PERF_FREQ         采样频率，默认 999 Hz
#   PYADAMAS_CC_FLAGS 追加编译参数（默认 -g -fno-omit-frame-pointer，便于栈展开）
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)

if [ $# -lt 1 ]; then
    echo "用法: $0 <用例名|文件路径>" >&2
    exit 1
fi
case_arg=$1
shift

if [ -f "$case_arg" ]; then
    src=$case_arg
else
    src=$REPO/bench/builtins/${case_arg%.py}.py
fi
if [ ! -f "$src" ]; then
    echo "找不到用例: $src" >&2
    exit 1
fi

if [ -n "${PYADAMAS_BIN:-}" ]; then
    compiler=$PYADAMAS_BIN
elif [ -x "$REPO/build/lead/pyadamas" ]; then
    compiler=$REPO/build/lead/pyadamas
elif [ -x /w/build/dev/pyadamas ]; then
    compiler=/w/build/dev/pyadamas
else
    echo "找不到 pyadamas，请设置 PYADAMAS_BIN" >&2
    exit 1
fi

command -v perf >/dev/null || { echo "找不到 perf" >&2; exit 1; }

work=${TMPDIR:-/tmp}/pya-prof
mkdir -p "$work"
name=$(basename "$src" .py)
bin=$work/$name.bin

export PYADAMAS_CC_FLAGS="${PYADAMAS_CC_FLAGS:--g -fno-omit-frame-pointer}"
"$compiler" build -o "$bin" "$src"
echo "已编译: $bin"

perf record -q -F "${PERF_FREQ:-999}" -g --call-graph fp \
    -o "$work/perf.data" -- "$bin" "$@" >/dev/null

echo
echo "==== self time（热点函数）===="
perf report -i "$work/perf.data" --stdio --no-children --sort symbol 2>/dev/null \
    | head -30 || true

echo
echo "==== 调用链（含子函数，top 20）===="
perf report -i "$work/perf.data" --stdio --children --sort symbol 2>/dev/null \
    | head -24 || true

echo
echo "原始数据: $work/perf.data（可用 perf report -i 交互查看，或 FlameGraph 生成火焰图）"
