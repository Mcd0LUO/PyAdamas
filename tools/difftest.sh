#!/usr/bin/env bash
# 差分测试：同一份源码在 -O0 / -O2 下输出必须一致。
# 用途：优化遍引入静默错误（野指针、越界、算错）时立刻抓出来。
#
# 并行：默认用 nproc 个 job。实测 188 用例串行 63s，32 核并行约 4s。
#   PYADAMAS_TEST_JOBS=8 bash tools/difftest.sh
#   PYADAMAS_TEST_FILTER='3[0-4]*' bash tools/difftest.sh   # 只跑匹配的
set -u
cd /home/lenovo/cpp/cpy23 || exit 1
PY=${1:-build/dev/pyadamas}
JOBS=${PYADAMAS_TEST_JOBS:-$(nproc 2>/dev/null || echo 4)}
FILTER=${PYADAMAS_TEST_FILTER:-}

match() {
    [ -z "$FILTER" ] && return 0
    case "$(basename "$1")" in
        $FILTER) return 0 ;;
        *) return 1 ;;
    esac
}

# 每个用例一个独立的临时目录：并行时不能共用 /tmp/dt0 /tmp/dt2。
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# 单个用例的检查：编译 O0/O2 各一次、运行各一次，比输出与退出码。
# 结果写成一行到 stdout：OK / DIFF<TAB>name<TAB>细节。
run_one() {
    local f=$1 idx=$2
    local name
    name=$(basename "$f" .py)
    local d="$WORK/$idx"
    mkdir -p "$d"
    local out0 out2 rc0 rc2
    out0=$(timeout -s KILL 20 "$PY" build -O0 -o "$d/o0" "$f" >/dev/null 2>&1 && timeout -s KILL 20 "$d/o0" 2>&1)
    rc0=$?
    out2=$(timeout -s KILL 20 "$PY" build -O2 -o "$d/o2" "$f" >/dev/null 2>&1 && timeout -s KILL 20 "$d/o2" 2>&1)
    rc2=$?
    if [ "$out0" != "$out2" ] || [ $rc0 -ne $rc2 ]; then
        printf 'DIFF\t%s\tO0 rc=%s: %s\tO2 rc=%s: %s\n' "$name" "$rc0" \
            "$(printf '%s' "$out0" | head -2 | tr '\n' ' ')" "$rc2" \
            "$(printf '%s' "$out2" | head -2 | tr '\n' ' ')"
    else
        printf 'OK\t%s\n' "$name"
    fi
}
export -f run_one
export PY WORK

# 收集用例（跳过 f* 反例组和 filter 不匹配的）。
LIST=$WORK/list.txt
: > "$LIST"
i=0
for f in tests/run/*.py; do
    name=$(basename "$f" .py)
    case "$name" in f*) continue ;; esac
    match "$f" || continue
    i=$((i+1))
    printf '%s\t%s\n' "$i" "$f" >> "$LIST"
done

if [ ! -s "$LIST" ]; then
    echo "差分测试: 0 个用例（filter 无匹配）"
    exit 0
fi

# 并行跑。xargs -P 控制并发；-n1 每次一行（idx + 路径）。
RESULTS=$WORK/results.txt
tr '\t' ' ' < "$LIST" | xargs -P "$JOBS" -n 2 bash -c 'run_one "$1" "$0"' > "$RESULTS"

fail=$(grep -c '^DIFF' "$RESULTS" || true)
n=$(wc -l < "$RESULTS")
if [ "$fail" -gt 0 ]; then
    grep '^DIFF' "$RESULTS" | while IFS=$'\t' read -r _ name o0 o2; do
        echo "  DIFF $name"
        echo "    $o0"
        echo "    $o2"
    done
fi
echo "差分测试: $n 个用例, $fail 个不一致"
exit $([ "$fail" -eq 0 ] && echo 0 || echo 1)
