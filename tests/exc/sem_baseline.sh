#!/usr/bin/env bash
# tests/exc/sem_baseline.sh —— 异常语义回归基线（setjmp 路径 vs 表驱动路径的对照）
#
# 用法:
#   bash tests/exc/sem_baseline.sh              # 跑默认的 42 个异常相关用例
#   bash tests/exc/sem_baseline.sh -v           # 失败时额外打印 output diff
#   PYADAMAS_BIN=/path/to/pyadamas bash tests/exc/sem_baseline.sh
#
# 退出码: 0 = 全部 PASS; 1 = 有 FAIL; 2 = 环境不满足（二进制缺失、用法错误）
#
# 选例方法（与 docs/exc-semantics.md 第 7 节一致）：
#   在 tests/run 里 grep try:/except/finally:/raise/with /.throw(/athrow(/StopIteration，
#   再人工确认覆盖了 docs/exc-semantics.md 第 2 节的语义分组。tests/fail、
#   tests/unsupported、tests/strict 里没有异常语义用例（它们的期望是编译期拒绝），
#   所以本基线只覆盖 tests/run。
#
# 约束:
#   - 只跑过滤后的子集（42 个），串行，< 2 分钟；不跑 tests/run_tests.sh 全量；
#   - 只用现成的 build/dev/pyadamas，不触发 cmake/ninja；
#   - 编译产物、期望输出、diff 全部落在 mktemp -d 目录，跑完删除，不碰 build/；
#   - 输出不含时间戳/随机数/临时路径，重复运行逐字节一致；
#   - 少数用例依赖 asyncio 的毫秒级 timer（24_async / 25_task / 27_runtime），
#     并发负载高时可能偶发 output diff；脚本对“output diff”自动重试一次，
#     重试结果不写进输出，所以稳定环境下输出仍然逐字节可复现。
set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
PY=${PYADAMAS_BIN:-$ROOT/build/dev/pyadamas}
VERBOSE=0

for arg in "$@"; do
    case "$arg" in
        -v|--verbose) VERBOSE=1 ;;
        -h|--help)
            sed -n '2,24p' "${BASH_SOURCE[0]}"
            exit 0
            ;;
        *)
            echo "usage: bash tests/exc/sem_baseline.sh [-v]" >&2
            exit 2
            ;;
    esac
done

if [ ! -x "$PY" ]; then
    echo "ERROR: pyadamas 可执行文件不存在或不可执行: $PY" >&2
    echo "       先准备 build/dev（或 PYADAMAS_BIN=/path/to/pyadamas 指定）" >&2
    exit 2
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/pyadamas-exc-baseline.XXXXXX") || exit 2
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT INT TERM

# ── 用例清单（顺序固定；分组对应 docs/exc-semantics.md 第 2 节）──────────
CASES=(
    # 2.1 try/except/else/finally 主干、raise、异常对象/链
    22_exception
    20_with
    159_exc_multi
    32_exception_value
    34_exception_chain
    157_rich_exc
    153_traceback
    # 2.6/2.5 帧展开资源回收、handler 寄存器 hazard
    92_exc_inline_rc
    100_handler_loop_carried
    # 2.4 生成器/协程/异步生成器（挂起、throw、close、finally）
    23_generator
    28_yield_from
    118_suspend_expr
    119_gen_except_suspend
    122_gen_throw
    123_finally_yield
    129_finally_jump
    130_throw_class
    94_suspend_positions
    93_gen_exc_rc
    # 2.4 asyncio / 异步语法
    24_async
    25_task
    26_async_lang
    29_async_comp
    31_asyncgen_proto
    124_asyncgen_await
    131_async_aiter
    154_nested_await
    # 2.3/2.6 运行时错误入口、内置函数、优化与 handler 的交互
    27_runtime
    33_builtins
    39_match_advanced
    43_indexing
    46_simd_reduce
    50_licm
    53_json
    58_re_err
    66_genexpr
    73_lazy_gen_range
    74_range_object
    77_functools
    91_csv
    126_decorated_slots
    145_collections_deque
)

# run_one <name> -> 0=PASS, 1=output diff, 2=编译/运行失败
# 结果写 $WORK/<name>.out，期望写 $WORK/<name>.exp。
run_one() {
    local name=$1
    local src="$ROOT/tests/run/$name.py"
    local out="$WORK/$name.out"
    local prog="$WORK/$name.prog"
    local rc=0

    if command -v timeout >/dev/null 2>&1; then
        if ( cd "$WORK" && timeout 120 "$PY" run --rebuild -o "$prog" "$src" ) >"$out" 2>&1; then
            rc=0
        else
            rc=$?
        fi
    else
        if ( cd "$WORK" && "$PY" run --rebuild -o "$prog" "$src" ) >"$out" 2>&1; then
            rc=0
        else
            rc=$?
        fi
    fi

    if [ "$rc" -ne 0 ]; then
        return 2
    fi

    # 与 tests/run_tests.sh 的 run_test 相同口径：从测试文件自身的期望段提取。
    sed -n '/^# --- expect ---$/,$p' "$src" | tail -n +2 | sed -E 's/^# ?//' > "$WORK/$name.exp"
    if diff -q "$WORK/$name.exp" "$out" >/dev/null 2>&1; then
        return 0
    fi
    return 1
}

pass=0
fail=0
failed=()

for name in "${CASES[@]}"; do
    src="$ROOT/tests/run/$name.py"
    if [ ! -f "$src" ]; then
        printf 'FAIL %s (missing %s)\n' "$name" "$src"
        failed+=("$name")
        fail=$((fail + 1))
        continue
    fi

    st=0
    run_one "$name" || st=$?
    # 只对 output diff 重试一次：容忍高并发负载下 asyncio 毫秒 timer 的偶发抖动。
    if [ "$st" -eq 1 ]; then
        st=0
        run_one "$name" || st=$?
    fi

    case "$st" in
        0)
            printf 'PASS %s\n' "$name"
            pass=$((pass + 1))
            ;;
        1)
            printf 'FAIL %s (output diff)\n' "$name"
            failed+=("$name")
            fail=$((fail + 1))
            if [ "$VERBOSE" -eq 1 ]; then
                echo "  ---- diff (expected vs actual, first 20 lines) ----"
                diff -u "$WORK/$name.exp" "$WORK/$name.out" | head -n 20 | sed 's/^/  | /'
            fi
            ;;
        *)
            printf 'FAIL %s (run failed)\n' "$name"
            failed+=("$name")
            fail=$((fail + 1))
            if [ "$VERBOSE" -eq 1 ]; then
                echo "  ---- output (tail) ----"
                tail -n 12 "$WORK/$name.out" | sed 's/^/  | /'
            fi
            ;;
    esac
done

total=$((pass + fail))
printf 'SUM PASS %d / %d FAIL %d\n' "$pass" "$total" "$fail"
if [ "$fail" -ne 0 ]; then
    printf 'FAILED:'
    printf ' %s' "${failed[@]}"
    echo
    exit 1
fi
exit 0
