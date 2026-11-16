#!/bin/bash
# tests/exc/z2_codegen/run.sh —— Z2-b2：--zero-cost-exc codegen 接线的一键验证
#
# 用法：bash tests/exc/z2_codegen/run.sh [build-dir] [pyadamas]
#   参数与 tests/run_tests.sh 一致；缺省 build/dev build/dev/pyadamas。
#
# 每个 cases/*.py 都用两条路径各编一次：
#   1) 老路径（默认 setjmp）：汇编里必须有 call _setjmp；
#   2) 新路径（--zero-cost-exc）：汇编里必须**没有** call _setjmp、没有
#      call pa_handler_push，并且出现 .cfi_personality / .cfi_lsda /
#      .section .gcc_except_table（catch-all = ff 9b 头部）；
#   3) 两条路径的 stdout+stderr 逐字节一致，并与用例末尾的
#      「# --- expect ---」逐字节一致；
#   4) 新路径产物：.eh_frame CIE 有 personality（readelf -wf 的 zPLR），
#      可执行文件里有 .gcc_except_table 段。
#
# 退出码：0 = 全过；1 = 有失败。
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
BUILD=${1:-$ROOT/build/dev}
PY=${2:-$BUILD/pyadamas}
if [ ! -x "$PY" ]; then
    echo "找不到 pyadamas 可执行文件：$PY"
    exit 1
fi

TMP=$(mktemp -d) || { echo "mktemp 失败"; exit 1; }
trap 'rm -rf "$TMP"' EXIT

FAILS=0
CHECKS=0
ok()  { CHECKS=$((CHECKS + 1)); printf '[PASS] %s\n' "$*"; }
bad() { CHECKS=$((CHECKS + 1)); FAILS=$((FAILS + 1)); printf '[FAIL] %s\n' "$*"; }

echo "==== z2_codegen（同一份 .py：setjmp 路径 vs --zero-cost-exc 路径）===="
echo "仓库根：$ROOT"
echo "pyadamas：$PY"
echo

for f in "$HERE"/cases/*.py; do
    name=$(basename "$f" .py)
    echo "-- $name"
    off_s="$TMP/$name.off.s"
    on_s="$TMP/$name.on.s"

    if ! "$PY" asm --no-zero-cost-exc "$f" > "$off_s" 2> "$TMP/$name.off.asm.err"; then
        bad "$name：老路径 asm 失败"
        sed -n '1,5p' "$TMP/$name.off.asm.err"
        continue
    fi
    if ! "$PY" asm --zero-cost-exc "$f" > "$on_s" 2> "$TMP/$name.on.asm.err"; then
        bad "$name：新路径 asm 失败"
        sed -n '1,5p' "$TMP/$name.on.asm.err"
        continue
    fi

    # ── 汇编结构断言 ────────────────────────────────────────────────
    off_sj=$(grep -c 'call _setjmp' "$off_s" || true)
    on_sj=$(grep -c 'call _setjmp' "$on_s" || true)
    on_push=$(grep -c 'call pa_handler_push' "$on_s" || true)
    off_pers=$(grep -c '\.cfi_personality' "$off_s" || true)
    on_pers=$(grep -c '\.cfi_personality' "$on_s" || true)
    on_lsda=$(grep -c '\.cfi_lsda' "$on_s" || true)
    on_table=$(grep -c '\.section \.gcc_except_table' "$on_s" || true)

    if [ "$off_sj" -ge 1 ]; then
        ok "$name：老路径有 call _setjmp（$off_sj 处，用例确实覆盖 try/except）"
    else
        bad "$name：老路径没有 call _setjmp，用例不覆盖目标形状"
    fi
    if [ "$off_pers" -eq 0 ]; then
        ok "$name：默认（关）不发射 .cfi_personality（开关默认关）"
    else
        bad "$name：默认路径出现了 .cfi_personality（开关没关住）"
    fi
    if [ "$on_sj" -eq 0 ]; then
        ok "$name：新路径没有 call _setjmp"
    else
        bad "$name：新路径仍有 $on_sj 处 call _setjmp"
    fi
    if [ "$on_push" -eq 0 ]; then
        ok "$name：新路径没有 call pa_handler_push"
    else
        bad "$name：新路径仍有 $on_push 处 call pa_handler_push"
    fi
    if [ "$on_pers" -ge 1 ] && [ "$on_lsda" -ge 1 ] && [ "$on_table" -ge 1 ]; then
        ok "$name：新路径有 .cfi_personality / .cfi_lsda / .gcc_except_table"
    else
        bad "$name：新路径的 LSDA 发射不完整（personality=$on_pers lsda=$on_lsda table=$on_table）"
    fi
    if grep -q '\.byte 0xff' "$on_s" && grep -q '\.byte 0x9b' "$on_s"; then
        ok "$name：LSDA 头部是 ff 9b（GCC 同款 catch-all 形态）"
    else
        bad "$name：LSDA 头部不是 ff 9b"
    fi

    # ── 运行行为断言 ────────────────────────────────────────────────
    off_prog="$TMP/$name.off.prog"
    on_prog="$TMP/$name.on.prog"
    off_out="$TMP/$name.off.out"
    on_out="$TMP/$name.on.out"
    "$PY" run --rebuild --no-zero-cost-exc -o "$off_prog" "$f" > "$off_out" 2>&1
    off_rc=$?
    "$PY" run --rebuild --zero-cost-exc -o "$on_prog" "$f" > "$on_out" 2>&1
    on_rc=$?

    if [ "$off_rc" -eq 0 ] && [ "$on_rc" -eq 0 ]; then
        ok "$name：两条路径都编译运行成功（rc = 0）"
    else
        bad "$name：运行失败（老路径 rc=$off_rc，新路径 rc=$on_rc）"
    fi
    if cmp -s "$off_out" "$on_out"; then
        ok "$name：两条路径 stdout 逐字节一致（$(wc -c < "$on_out") 字节）"
    else
        bad "$name：两条路径 stdout 不一致"
        diff "$off_out" "$on_out" | head -12
    fi
    sed -n '/^# --- expect ---$/,$p' "$f" | tail -n +2 | sed -E 's/^# ?//' > "$TMP/$name.exp"
    if cmp -s "$TMP/$name.exp" "$on_out"; then
        ok "$name：新路径输出与用例 expect 逐字节一致"
    else
        bad "$name：新路径输出与 expect 不一致"
        diff "$TMP/$name.exp" "$on_out" | head -12
    fi

    # ── 产物级断言（.eh_frame / .gcc_except_table）──────────────────
    if readelf -S "$on_prog" 2>/dev/null | grep -q '\.gcc_except_table'; then
        ok "$name：新路径可执行文件里有 .gcc_except_table 段"
    else
        bad "$name：新路径可执行文件里没有 .gcc_except_table 段"
    fi
    if readelf -wf "$on_prog" 2>/dev/null | grep -q 'zPLR'; then
        ok "$name：新路径 .eh_frame CIE augmentation 含 personality/LSDA（zPLR）"
    else
        bad "$name：新路径 .eh_frame CIE 里没有 zPLR"
    fi
    echo
done

echo "==== 结果：$((CHECKS - FAILS))/$CHECKS PASS ===="
if [ "$FAILS" -ne 0 ]; then
    echo "FAIL"
    exit 1
fi
echo "PASS"
exit 0
