#!/usr/bin/env bash
# checkall.sh —— PyAdamas 一键全闸门 + 性能基线回归告警
#
# 用法：
#   bash tools/checkall.sh                 # --full（默认）：全部闸门
#   bash tools/checkall.sh --quick         # 只跑套件 + difftest + check_size
#   bash tools/checkall.sh --perf          # 只采集/比对 -O2 指令数基线
#   bash tools/checkall.sh --write-baseline  # !! 人工确认后才用：覆盖基线文件
#   bash tools/checkall.sh --jobs 8        # 覆盖并行度（默认 nproc，上限 16）
#
# 环境变量：
#   PYADAMAS_BIN     编译器路径（默认 $ROOT/build/lead/pyadamas）
#   SUITE_BUILD_DIR  套件/difftest 的构建目录（默认 $ROOT/build/gfix）
#   CHECKALL_SKIP_BUILD=1  跳过第 1 步增量构建
#   CHECKALL_WARN_PCT / CHECKALL_FAIL_PCT / CHECKALL_PERF_REPS 阈值与轮数
#
# 并发安全：
#   * difftest 始终 flock /tmp/pyadamas-difftest.lock（它用共享 /tmp 目录和
#     modcache，两个 difftest 并发会互相污染出假 timeout）；
#   * 套件用可配置构建目录 SUITE_BUILD_DIR，默认 build/gfix；多人同时跑闸门
#     时给每个人传不同的 SUITE_BUILD_DIR，并用 flock /tmp/pyadamas-checkall.lock
#     包住整条命令以串行化构建/测量。
#
# 任一项失败返回非零；汇总表含每项耗时与总耗时。
set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT" || exit 2

BIN=${PYADAMAS_BIN:-$ROOT/build/lead/pyadamas}
BUILD_DIR=${SUITE_BUILD_DIR:-$ROOT/build/gfix}
BUILD_JOBS=${BUILD_JOBS:-4}
BASELINE=$ROOT/tools/perf_baseline.txt
WARN_PCT=${CHECKALL_WARN_PCT:-2}
FAIL_PCT=${CHECKALL_FAIL_PCT:-5}
# 20 轮取最小：本机短基准有双峰分布（同一二进制 instructions 能差 40%），
# 实测 3 轮会误报 +5%，20 轮取最小才能稳定到 ±0.3%（见 docs/testing.md）。
PERF_REPS=${CHECKALL_PERF_REPS:-3}
JOBS_RAW=${PYADAMAS_TEST_JOBS:-}
MODE=full
WRITE_BASELINE=0
SKIP_BUILD=0
STRICT_GIT=${CHECKALL_STRICT_GIT:-0}
ASAN=0

usage() {
    cat <<'EOF'
用法: bash tools/checkall.sh [--full|--quick|--perf] [--jobs N] [--no-build] [--write-baseline] [--asan]
  --full        （默认）1构建 2套件 3difftest 4check_size 5基准12x3 6指令数 7SIMD 8LSP 9git状态
  --quick       1-4（日常快速回归）
  --perf        6：指令数采集 + 基线比对（20 轮取最小，instructions:u）
  --jobs N      并行度（默认 16，上限 16）
  --no-build    跳过构建（已手工构建过时）
  --write-baseline !! 人工确认后才用：把当前指令数覆盖写入 tools/perf_baseline.txt
  --asan        显式 opt-in：在套件阶段开 ASan（默认关！）。
                警告：仅当改动触及 runtime 内存路径（RC/GC/EH/容器/字符串/文件）或 lowering
                所有权/cleanup 时使用，且建议用 PYADAMAS_TEST_FILTER 只跑相关用例。
                非此类改动不要开；--full/--perf 的指令数告警不会自动触发 ASan。
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --full) MODE=full ;;
        --quick) MODE=quick ;;
        --perf) MODE=perf ;;
        --jobs) shift; JOBS_RAW=${1:?--jobs 需要一个数字} ;;
        --jobs=*) JOBS_RAW=${1#*=} ;;
        --no-build) SKIP_BUILD=1 ;;
        --asan) ASAN=1 ;;
        --write-baseline) WRITE_BASELINE=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "checkall: 未知参数 $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

NPROC=$(nproc 2>/dev/null || echo 4)
# 默认 16：机器 48 核但别打满；覆盖用 --jobs N 或 PYADAMAS_TEST_JOBS。
JOBS=${JOBS_RAW:-16}
if ! [[ $JOBS =~ ^[0-9]+$ ]] || [ "$JOBS" -lt 1 ]; then
    echo "checkall: --jobs 必须是正整数" >&2; exit 2
fi
if [ "$JOBS" -gt 16 ]; then
    echo "checkall: --jobs $JOBS 超过上限 16，按 16 处理" >&2
    JOBS=16
fi
[ "${CHECKALL_SKIP_BUILD:-0}" = 1 ] && SKIP_BUILD=1

# ── 结果收集 ───────────────────────────────────────────────────
NAMES=(); STATUSES=(); SECS=(); DETAILS=()
overall=0
record() {
    local name=$1 secs=$2 rc=$3 detail=${4:-} cmd=${5:-}
    NAMES+=("$name"); SECS+=("$secs"); STATUSES+=("$rc"); DETAILS+=("$detail")
    if [ "$rc" -eq 0 ]; then
        printf '[PASS] %-18s %7ss  %s\n' "$name" "$secs" "$detail"
    else
        printf '[FAIL] %-18s %7ss  %s\n' "$name" "$secs" "$detail"
        [ -n "$cmd" ] && printf '       重跑: %s\n' "$cmd"
    fi
    [ "$rc" -ne 0 ] && overall=1
    return 0
}
fmt_time() {
    awk -v s="$1" 'BEGIN{printf "%d:%05.2f", int(s/60), s%60}'
}
summary() {
    echo
    echo "================ 汇总（jobs=$JOBS，模式=$MODE）================"
    printf '%-22s %8s %10s  %s\n' '闸门' '状态' '耗时' '备注'
    printf '%-22s %8s %10s  %s\n' '------' '----' '----' '----'
    local i total=0 st
    for i in "${!NAMES[@]}"; do
        total=$(awk -v a="$total" -v b="${SECS[$i]}" 'BEGIN{print a+b}')
        case "${STATUSES[$i]}" in
            0) st=PASS ;;
            2) st=WARN ;;
            *) st=FAIL ;;
        esac
        printf '%-22s %8s %10s  %s\n' "${NAMES[$i]}" "$st" "$(fmt_time "${SECS[$i]}")" "${DETAILS[$i]}"
    done
    printf '%-22s %8s %10s\n' 'TOTAL' '' "$(fmt_time "$total")"
    if [ "$overall" -ne 0 ]; then
        echo "结果：有闸门失败（非零退出）。"
    else
        echo "结果：全部通过。"
    fi
}

timer_start() { date +%s.%N; }
timer_end() {
    local t0=$1 t1
    t1=$(date +%s.%N)
    awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.2f", b-a}'
}

if [ ! -x "$BIN" ]; then
    echo "checkall: 找不到编译器 $BIN；先 cmake --build build/lead 或设 PYADAMAS_BIN" >&2
    exit 2
fi

# ── 性能采集辅助函数 ────────────────────────────────────────────
BENCH_NAMES=(binary_trees collatz fannkuch fasta fib mandelbrot matmul nbody nqueens sieve spectral_norm wordfreq)
PERF_TMP=""
perf_cleanup() {
    [ -n "$PERF_TMP" ] && [ -d "$PERF_TMP" ] && rm -rf "$PERF_TMP"
}
trap perf_cleanup EXIT

# measure_one <name> <outfile>：用当前 $BIN 编译 -O2 --rebuild，$PERF_REPS 轮 perf stat
# 取最小。必须用 instructions:u —— 计数含内核指令会制造假差异（内核路径随负载变化）。
# instructions:u 本身是确定性的（同一二进制重复跑只差几条），3 轮取最小即可；
# 绑核/固定 MALLOC_ARENA_MAX 只是让个别分配相关路径更稳定，不影响阈值判断。
PIN_CPU=$(awk '/Cpus_allowed_list/ {print $2}' /proc/self/status 2>/dev/null | tr ',' '\n' | head -1 | cut -d- -f1)
PIN_CPU=${PIN_CPU:-0}
measure_one() {
    local name=$1 outfile=$2 i val best=""
    local src="$ROOT/bench/compare/$name.py"
    local bin="$PERF_TMP/$name.bin"
    if ! "$BIN" build --rebuild -O2 -o "$bin" "$src" >/dev/null 2>&1; then
        return 1
    fi
    for i in $(seq 1 "$PERF_REPS"); do
        if ! MALLOC_ARENA_MAX=1 taskset -c "$PIN_CPU" perf stat -e instructions:u -x, -- "$bin" >/dev/null 2>"$PERF_TMP/perf.$name.$i"; then
            return 1
        fi
        # perf stat -x, 输出：value,unit,event,run_time,percent,...
        # $1 才是指令数；$4 是 run_time(ns)（曾误取 $4，把墙钟噪声当成指令数）。
        val=$(awk -F, '$3 == "instructions:u" {print $1; exit}' "$PERF_TMP/perf.$name.$i")
        case "$val" in
            ''|*[!0-9]*) return 1 ;;
        esac
        if [ -z "$best" ] || [ "$val" -lt "$best" ]; then best=$val; fi
    done
    echo "$best" > "$outfile"
    return 0
}

collect_perf() {
    local t0 rc=0 name
    t0=$(timer_start)
    echo
    echo "──── 性能基线采集（$PERF_REPS 轮取最小，instructions:u）────"
    for name in "${BENCH_NAMES[@]}"; do
        if ! measure_one "$name" "$PERF_TMP/$name.val"; then
            echo "  $name: 采集失败"
            rc=1
        else
            printf '  %-16s %14s instructions\n' "$name" "$(cat "$PERF_TMP/$name.val")"
        fi
    done
    PERF_COLLECT_RC=$rc
    PERF_COLLECT_SECS=$(timer_end "$t0")
    return 0
}

write_baseline() {
    collect_perf
    if [ "$PERF_COLLECT_RC" -ne 0 ]; then
        echo "checkall: 性能采集有失败，拒绝写基线" >&2
        record "write-baseline" "$PERF_COLLECT_SECS" 1 "采集失败"
        return 0
    fi
    local tmp="$BASELINE.tmp.$$"
    {
        echo "# PyAdamas -O2 instructions:u 性能基线（perf stat，$PERF_REPS 轮最小，绑核 + MALLOC_ARENA_MAX=1）"
        echo "# 写入时间: $(date -Is)  HEAD: $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
        echo "# 同一二进制跑两次也可能差 ±0.3%；阈值 +2% 警告 / +5% 失败；负数（变好）不算失败。"
        echo "# 只在编译器/代码生成/运行时发生有意变更、并人工确认后运行 --write-baseline。"
        local name
        for name in "${BENCH_NAMES[@]}"; do
            printf '%s %s\n' "$name" "$(cat "$PERF_TMP/$name.val")"
        done
    } > "$tmp"
    mv "$tmp" "$BASELINE"
    echo "基线已写入 $BASELINE"
    record "write-baseline" "$PERF_COLLECT_SECS" 0 "已覆盖"
}

compare_perf() {
    local rc=0 name base cur pct kind warn=0 fail=0 painted
    collect_perf
    rc=$PERF_COLLECT_RC
    echo
    echo "──── 性能基线比对（阈值 +$WARN_PCT% 警告 / +$FAIL_PCT% 失败）────"
    printf '%-18s %14s %14s %10s  %s\n' '基准' '基线' '当前' '变化' '状态'
    printf '%-18s %14s %14s %10s  %s\n' '------' '----' '----' '----' '----'
    local RED='' YELLOW='' RESET=''
    if [ -t 1 ]; then RED=$'\033[31m'; YELLOW=$'\033[33m'; RESET=$'\033[0m'; fi
    while read -r name base _; do
        case "$name" in ''| '#'*) continue ;; esac
        [ -z "${base:-}" ] && continue
        if [ ! -f "$PERF_TMP/$name.val" ]; then
            printf '%-18s %14s %14s %10s  %s\n' "$name" "$base" 'MISSING' '-' 'FAIL'
            fail=$((fail+1)); rc=1; continue
        fi
        cur=$(cat "$PERF_TMP/$name.val")
        pct=$(awk -v a="$base" -v b="$cur" 'BEGIN{printf "%.2f", (b-a)*100/a}')
        kind=OK
        if [ "$base" -le 0 ]; then
            kind=FAIL
        elif awk -v p="$pct" -v t="$WARN_PCT" 'BEGIN{exit !(p>t)}'; then
            if awk -v p="$pct" -v t="$FAIL_PCT" 'BEGIN{exit !(p>t)}'; then
                kind=FAIL; fail=$((fail+1)); rc=1
            else
                kind=WARN; warn=$((warn+1))
            fi
        fi
        painted=$kind
        case "$kind" in
            FAIL) painted="${RED}$kind${RESET}" ;;
            WARN) painted="${YELLOW}$kind${RESET}" ;;
        esac
        printf '%-18s %14s %14s %+9.2f%%  %b\n' "$name" "$base" "$cur" "$pct" "$painted"
    done < "$BASELINE"
    echo "性能比对: warn=$warn fail=$fail"
    record "perf-baseline" "$PERF_COLLECT_SECS" "$rc" "warn=$warn fail=$fail" "bash tools/checkall.sh --perf"
}

# ── 各闸门实现 ──────────────────────────────────────────────────
gate_build() {
    local t0 rc
    if [ "$SKIP_BUILD" -eq 1 ]; then
        record "build" 0 0 "skipped (--no-build)" ""
        return 0
    fi
    t0=$(timer_start)
    echo "──── [1] 增量构建 build/lead ────"
    cmake --build "$ROOT/build/lead" -j"$BUILD_JOBS"
    rc=$?
    record "build" "$(timer_end "$t0")" "$rc" "cmake -j$BUILD_JOBS" "cmake --build build/lead -j$BUILD_JOBS"
    return 0
}

gate_suite() {
    local t0 rc logfile
    logfile=$(mktemp /tmp/checkall-suite.XXXXXX)
    t0=$(timer_start)
    local asan_env=""
    [ "$ASAN" -eq 1 ] && asan_env="PYADAMAS_TEST_ASAN=1"
    echo "──── [2] 测试套件（PYADAMAS_TEST_JOBS=$JOBS，build dir=$BUILD_DIR，ASan=$ASAN）────"
    [ "$ASAN" -eq 1 ] && echo "  警告：显式 ASan 模式，仅用于 runtime 内存路径改动；建议配合 PYADAMAS_TEST_FILTER。"
    mkdir -p "$BUILD_DIR"
    env $asan_env PYADAMAS_TEST_JOBS="$JOBS" bash "$ROOT/tests/run_tests.sh" "$BUILD_DIR" "$BIN" 2>&1 | tee "$logfile"
    rc=${PIPESTATUS[0]}
    local passed failed
    passed=$(grep -c 'ran ok' "$logfile" || true)
    failed=$(grep -cE 'FAILED|OUTPUT DIFF|MISSING RESULT|WRONG:|MISSED|COMPILED|NOT MARKED|CACHE MISS|SECOND RUN' "$logfile" || true)
    record "suite" "$(timer_end "$t0")" "$rc" "$passed ran ok / $failed 失败" "PYADAMAS_TEST_JOBS=$JOBS $asan_env bash tests/run_tests.sh $BUILD_DIR $BIN"
    rm -f "$logfile"
    return 0
}

gate_difftest() {
    local t0 rc logfile
    logfile=$(mktemp /tmp/checkall-diff.XXXXXX)
    t0=$(timer_start)
    echo "──── [3] difftest（独占 flock，JOBS=$JOBS）────"
    if command -v flock >/dev/null 2>&1; then
        PYADAMAS_TEST_JOBS="$JOBS" flock /tmp/pyadamas-difftest.lock \
            bash "$ROOT/tools/difftest.sh" "$BIN" 2>&1 | tee "$logfile"
        rc=${PIPESTATUS[0]}
    else
        echo "flock 不可用：拒绝并发跑 difftest" >&2
        rc=2
    fi
    record "difftest" "$(timer_end "$t0")" "$rc" "$(grep '个用例' "$logfile" | tail -1 || echo 'flock/执行失败')" "flock /tmp/pyadamas-difftest.lock env PYADAMAS_TEST_JOBS=$JOBS bash tools/difftest.sh $BIN"
    rm -f "$logfile"
    return 0
}

gate_size() {
    local t0 rc
    t0=$(timer_start)
    echo "──── [4] check_size ────"
    bash "$ROOT/tests/check_size.sh"
    rc=$?
    record "check_size" "$(timer_end "$t0")" "$rc" "" "bash tests/check_size.sh"
    return 0
}

gate_bench_modes() {
    local t0 rc=0 n o exp got src bin
    t0=$(timer_start)
    echo "──── [5] 12 基准 × 3 档输出比对（CPython 逐字节）────"
    for n in "${BENCH_NAMES[@]}"; do
        src="$ROOT/bench/compare/$n.py"
        exp=$(python3 "$src" 2>&1)
        for o in 0 1 2; do
            bin="$PERF_TMP/bench-$n-O$o"
            local cmd="$BIN build --rebuild -O$o -o $bin $src"
            if ! "$BIN" build --rebuild -"$o" -o "$bin" "$src" >/dev/null 2>&1; then
                echo "  [FAIL] $n -O$o: BUILD FAILED"
                echo "         重跑: $cmd"
                rc=1; continue
            fi
            got=$("$bin" 2>&1)
            if [ "$got" = "$exp" ]; then
                echo "  [OK] $n -O$o"
            else
                echo "  [FAIL] $n -O$o: OUTPUT DIFF"
                echo "         CPython:  $(printf '%s' "$exp" | head -3 | tr '\n' ';')"
                echo "         PyAdamas: $(printf '%s' "$got" | head -3 | tr '\n' ';')"
                echo "         重跑: $cmd && diff <($bin 2>&1) <(python3 $src 2>&1)"
                rc=1
            fi
        done
    done
    record "bench-12x3" "$(timer_end "$t0")" "$rc" "$([ $rc -eq 0 ] && echo '36/36 逐字节一致' || echo '有差异')" "bash tools/checkall.sh --full"
    return 0
}

gate_lsp() {
    local t0 rc
    t0=$(timer_start)
    echo "──── [6] LSP 冒烟（PYADAMAS=$BIN）────"
    PYADAMAS="$BIN" bash "$ROOT/tests/lsp/smoke.sh"
    rc=$?
    record "lsp-smoke" "$(timer_end "$t0")" "$rc" "" "PYADAMAS=$BIN bash tests/lsp/smoke.sh"
    return 0
}

gate_simd() {
    local t0 rc=0 n bin val
    t0=$(timer_start)
    echo "──── [7] SIMD 变体指令数（PYADAMAS_SIMD=1，仅打印）────"
    for n in matmul spectral_norm; do
        bin="$PERF_TMP/simd-$n"
        if ! PYADAMAS_SIMD=1 "$BIN" build --rebuild -O2 -o "$bin" "$ROOT/bench/compare/$n.py" >/dev/null 2>&1; then
            echo "  $n: 编译失败"; rc=1; continue
        fi
        if ! MALLOC_ARENA_MAX=1 taskset -c "$PIN_CPU" perf stat -e instructions:u -x, -- "$bin" >/dev/null 2>"$PERF_TMP/simd-$n.perf"; then
            echo "  $n: perf 失败"; rc=1; continue
        fi
        val=$(awk -F, '$3 == "instructions:u" {print $1; exit}' "$PERF_TMP/simd-$n.perf")
        echo "  $n (SIMD=1): $val instructions"
        case "$val" in
            ''|*[!0-9]*) echo "  $n: 指令数非法"; rc=1 ;;
        esac
    done
    record "simd-variants" "$(timer_end "$t0")" "$rc" "打印量级" "PYADAMAS_SIMD=1 $BIN build --rebuild -O2 -o /tmp/simd_mm bench/compare/matmul.py"
    return 0
}

# ── 闸门 9：工作区干净检查 ──────────────────────────────────────
gate_git() {
    local t0 rc=0 out n=0
    t0=$(timer_start)
    echo "──── [9] 工作区干净检查（git status --short）────"
    out=$(git -C "$ROOT" status --short)
    if [ -n "$out" ]; then
        n=$(printf '%s\n' "$out" | wc -l)
        printf '%s\n' "$out" | head -30
        echo "  （共 $n 项变更）"
        if [ "$STRICT_GIT" = 1 ]; then
            rc=1
        else
            echo "  提示：多代理会话里工作区预期是脏的；设 CHECKALL_STRICT_GIT=1 可让它失败。"
        fi
    else
        echo "  （干净）"
    fi
    record "git-status" "$(timer_end "$t0")" "$rc" "$n 项变更" "git -C $ROOT status --short"
}
# ── 主流程 ──────────────────────────────────────────────────────
T0=$(timer_start)
PERF_TMP=$(mktemp -d /tmp/checkall-perf.XXXXXX)

if [ "$WRITE_BASELINE" -eq 1 ]; then
    echo "#################################################################"
    echo "# 警告：--write-baseline 会覆盖 $BASELINE。"
    echo "# 只有人工确认「这次变更是有意的性能变化」后才可使用。"
    echo "# 同一二进制两次运行也有 ±0.3% 噪声，不要因为一次红就改基线。"
    echo "#################################################################"
    write_baseline
    summary
    exit "$overall"
fi

case "$MODE" in
    perf)
        compare_perf
        summary
        exit "$overall"
        ;;
    quick)
        gate_build
        gate_suite
        gate_difftest
        gate_size
        ;;
    full)
        gate_build
        gate_suite
        gate_difftest
        gate_size
        gate_bench_modes
        gate_lsp
        gate_simd
        gate_git
        compare_perf
        ;;
esac

summary
exit "$overall"
