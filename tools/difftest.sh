#!/usr/bin/env bash
# 差分测试：同一份源码在 -O0 / -O2 下输出必须一致。
# 用途：优化遍引入静默错误（野指针、越界、算错）时立刻抓出来。
#
# 并行：默认用 nproc 个 job；实测 440 用例、48 路约 30s。
# 首轮报 DIFF/BUILDFAIL 的用例会自动**单路复跑一次**（超时 20s → 90s）：
# 复跑一致的算「抖动」并单独列出，不算失败——真差异是确定性的，复跑仍报错。
#   PYADAMAS_TEST_JOBS=8 bash tools/difftest.sh
#   PYADAMAS_TEST_FILTER='3[0-4]*' bash tools/difftest.sh   # 只跑匹配的
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1
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
# 结果写成一行到 stdout：
#   OK<TAB>name                         两侧都编过、都跑过、输出与退出码一致
#   DIFF<TAB>name<TAB>细节              两侧都编过，运行结果不同
#   BUILDFAIL<TAB>name<TAB>细节         至少一侧编不过
# 编译与运行分开记退出码：旧写法用 `build && run` 串联，两侧**都编不过**时
# 输出都是空、退出码都是 1，会被判成「一致」——正是 HANDOFF §6.5 记的假绿
# （0.288 的 movzbq 写错寄存器，difftest 193/0 全绿，套件却从 211 掉到 189）。
run_one() {
    local f=$1 idx=$2
    local name
    name=$(basename "$f" .py)
    local d="$WORK/$idx"
    mkdir -p "$d"
    local out0 out2 rc0 rc2 extra b0 b2 tmo
    # 串行复核时放宽超时（DT_TIMEOUT=90）：并行下 20s 超时是假 DIFF 的主因。
    tmo=${DT_TIMEOUT:-20}
    # 用例级编译开关（与 tests/run_tests.sh 同一约定）：文件头声明
    # 「# pyadamas: type-tags」时以 PYADAMAS_TYPE_TAGS=1 编译。
    extra=""
    if sed -n '1,8p' "$f" | grep -q '^# pyadamas: type-tags'; then
        extra="PYADAMAS_TYPE_TAGS=1"
    fi
    # P4 语言缺口（与 run_tests.sh 同一约定）。
    if sed -n '1,8p' "$f" | grep -q '^# pyadamas: lang-gaps'; then
        extra="PYADAMAS_LANG_GAPS=1"
    fi
    env $extra timeout -s KILL "$tmo" "$PY" build -O0 -o "$d/o0" "$f" >/dev/null 2>"$d/b0"
    b0=$?
    env $extra timeout -s KILL "$tmo" "$PY" build -O2 -o "$d/o2" "$f" >/dev/null 2>"$d/b2"
    b2=$?
    if [ $b0 -ne 0 ] || [ $b2 -ne 0 ]; then
        printf 'BUILDFAIL\t%s\tO0 build rc=%s: %s\tO2 build rc=%s: %s\n' "$name" \
            "$b0" "$(head -1 "$d/b0" | tr -d '\t')" "$b2" "$(head -1 "$d/b2" | tr -d '\t')"
        return
    fi
    out0=$(timeout -s KILL "$tmo" "$d/o0" 2>&1)
    rc0=$?
    out2=$(timeout -s KILL "$tmo" "$d/o2" 2>&1)
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
    # f<数字>*：反例组命名（预期编译失败；真正的反例都在 tests/fail/，
    # 这里只是防御）。**不能写成 f***：2026-11-25 起新增用例不再编号，
    # 描述性名字可能以 f 开头（fnmatch_anchor 就被误跳过过一次）。
    # 180_codepoint_perf 打印 wall-clock 时间，-O0/-O2 计时天然不同，不适合
    # 做输出比对。233_json_perf 只钉正确性（不打印时间），参与差分。
case "$name" in f[0-9]*) continue ;; 180_codepoint_perf) continue ;; esac
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

# 串行复核首轮不一致的用例：真差异是确定性的，复跑仍是 DIFF；并行下 20s
# 超时与资源竞争造成的假 DIFF 会在单路 + 90s 下消失。BUILDFAIL 同样复核，
# 避免把「环境抖动导致的编译失败」当成回归。
RETRY=$WORK/retry.txt
: > "$RETRY"
if grep -qE '^(DIFF|BUILDFAIL)' "$RESULTS"; then
    grep -E '^(DIFF|BUILDFAIL)' "$RESULTS" | cut -f2 | sort -u |
    while IFS= read -r name; do
        [ -f "tests/run/$name.py" ] || continue
        DT_TIMEOUT=90 run_one "tests/run/$name.py" "retry-$name" >> "$RETRY"
    done
fi

n=$(wc -l < "$RESULTS")
real=$(grep -cE '^(DIFF|BUILDFAIL)' "$RETRY" || true)
flake=$(grep -c '^OK' "$RETRY" || true)
if [ "$flake" -gt 0 ]; then
    echo "  注意：$flake 个用例首轮不一致、串行复跑一致（疑似并行超时/资源竞争）："
    grep '^OK' "$RETRY" | cut -f2 | sed 's/^/    /'
fi
if [ "$real" -gt 0 ]; then
    grep -E '^(DIFF|BUILDFAIL)' "$RETRY" | while IFS=$'\t' read -r kind name o0 o2; do
        echo "  $kind $name"
        echo "    $o0"
        echo "    $o2"
    done
fi
suffix=""
[ "$flake" -gt 0 ] && suffix="（另有 $flake 个首轮抖动已在复跑中排除）"
echo "差分测试: $n 个用例, $real 个不一致$suffix"
exit $([ "$real" -eq 0 ] && echo 0 || echo 1)
