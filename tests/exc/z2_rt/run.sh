#!/bin/bash
# tests/exc/z2_rt/run.sh —— Z2-b1（零开销异常的运行时胶水 + 方案 G 帧清理）一键验证
#
#   用法：bash tests/exc/z2_rt/run.sh
#   退出码：0 = 全部通过；1 = 有失败
#
# 验证内容（不碰仓库的 CMake/ninja 构建；运行时各部分用 gcc 直接编成 .o 再链接，
# 构建产物放 mktemp 临时目录，退出即删）：
#   1. runtime/rt_eh.c、runtime/rt_personality.c 严格编译（-O2 -Wall -Wextra -Werror）；
#   2. 运行时（rt.o 的全部组成，personality/eh 打开 -DPA_PERSONALITY_TRACE）
#      + driver.c + 手写汇编夹具 z2_threelayer.S 链接成测试程序，-no-pie 与 PIE 两套；
#   3. 断言（见 driver.c 的用例清单）：
#      - 三层帧：inner 抛 -> mid 有 3 个槽（owned / owned|bit0 / 只给 GC 扫的 bit1）
#        -> outer catch-all。phase 2 的 handler 帧里批量释放比它更深的记录：
#        inner/mid 的 owned 槽各被释放一次、bit1 不释放、handler 帧自己的记录与
#        槽保留（handler 正常出口才释放）；
#      - landing pad 里 pa_eh_landing 装出的全局状态与 longjmp 落地后逐项一致
#        （pending_exc / 富实例 / 多参 args 搬回，current_exc / exc_save_top 不动）；
#      - handler 帧没有记录时，释放停止点是「到 handler 帧的 CFA」：链上更外层的
#        记录保留（等价性证明的另一半）；
#      - 未捕获路径：pa_eh_throw 返回 PA_EH_UNCAUGHT、phase 2 不跑、不释放任何帧；
#      - 10 万次 throw/catch 后 RSS 增量 <= 1024 KB（GC 开、PYADAMAS_GC_DISABLE=1 两种）；
#      - personality 观测计数器：每次捕获释放调用 1 次、释放 2 条记录，
#        每次抛出落地 1 次、收尾 1 次、展开包装不泄漏；
#   4. 反证（证明上面这套断言不是空转）：把夹具用 -DZ2_NEGATIVE=1 编成「中间帧
#      不压帧记录」的变体 —— 没有记录可释放时，帧释放计数断言必须失败；再用
#      Z2_NEG_KEEP_GOING=1 把 10 万次跑完，RSS 判据必须报出 > 1024 KB 的增量。
#
# 依赖：Linux x86-64、gcc、binutils（readelf）。

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
CC=gcc
CC_ENV=$(printenv CC)
if [ -n "$CC_ENV" ]; then CC=$CC_ENV; fi
BUILD=$(mktemp -d) || { echo "mktemp 失败"; exit 1; }
trap 'rm -rf "$BUILD"' EXIT

FAILS=0
CHECKS=0

ok()  { printf '[PASS] %s\n' "$*"; }
bad() { printf '[FAIL] %s\n' "$*"; FAILS=$((FAILS + 1)); }

# 运行时的全部组成（personality / eh 单独带 trace 编，见 build_suite）。
RT_PARTS=""
for f in "$ROOT"/runtime/rt_*.c; do
    base=$(basename "$f" .c)
    case "$base" in
        rt_personality|rt_eh) continue ;;
    esac
    RT_PARTS="$RT_PARTS $base"
done

echo "==== z2_rt（Z2-b1：运行时胶水 + 方案 G 帧清理）===="
echo "仓库根：$ROOT"
echo "构建目录：$BUILD"
echo

# ── 1) 严格编译 ─────────────────────────────────────────────────────
if "$CC" -O2 -Wall -Wextra -Werror -I "$ROOT/runtime" \
        -c "$ROOT/runtime/rt_eh.c" -o "$BUILD/rt_eh_strict.o"; then
    ok "runtime/rt_eh.c 严格编译（-O2 -Wall -Wextra -Werror -I runtime）"
else
    bad "runtime/rt_eh.c 严格编译失败"
fi
if "$CC" -O2 -Wall -Wextra -Werror -I "$ROOT/runtime" \
        -c "$ROOT/runtime/rt_personality.c" -o "$BUILD/rt_personality_strict.o"; then
    ok "runtime/rt_personality.c 严格编译"
else
    bad "runtime/rt_personality.c 严格编译失败"
fi

# ── 2) 构建 ─────────────────────────────────────────────────────────
# build_suite <目录> <CFLAGS 后缀> <链接参数>
build_suite() {
    local dir=$1 cflags=$2 lflags=$3
    local base objs
    mkdir -p "$dir" || return 1
    for base in $RT_PARTS; do
        "$CC" $cflags -I "$ROOT/runtime" -c "$ROOT/runtime/$base.c" \
            -o "$dir/$base.o" || return 1
    done
    "$CC" $cflags -DPA_PERSONALITY_TRACE -I "$ROOT/runtime" \
        -c "$ROOT/runtime/rt_personality.c" -o "$dir/rt_personality.o" || return 1
    "$CC" $cflags -DPA_PERSONALITY_TRACE -I "$ROOT/runtime" \
        -c "$ROOT/runtime/rt_eh.c" -o "$dir/rt_eh.o" || return 1
    "$CC" $cflags -DPA_PERSONALITY_TRACE -I "$ROOT/runtime" \
        -c "$HERE/driver.c" -o "$dir/driver.o" || return 1
    "$CC" -O2 -g -c "$HERE/z2_threelayer.S" -o "$dir/z2_fixture.o" || return 1
    "$CC" -O2 -g -c -DZ2_NEGATIVE=1 "$HERE/z2_threelayer.S" \
        -o "$dir/z2_fixture_neg.o" || return 1
    objs=""
    for base in $RT_PARTS; do objs="$objs $dir/$base.o"; done
    "$CC" $lflags -o "$dir/z2_rt" "$dir/driver.o" "$dir/z2_fixture.o" \
        "$dir/rt_eh.o" "$dir/rt_personality.o" $objs -lm || return 1
    "$CC" $lflags -o "$dir/z2_rt_neg" "$dir/driver.o" "$dir/z2_fixture_neg.o" \
        "$dir/rt_eh.o" "$dir/rt_personality.o" $objs -lm || return 1
}

# check_fixture_shape <对象文件> <标签>
check_fixture_shape() {
    local obj=$1 label=$2
    if readelf -x .gcc_except_table "$obj" | grep -q 'ff9b'; then
        ok "$label：夹具 LSDA 头为 ff 9b（GCC 同款 catch-all 形态）"
    else
        bad "$label：夹具 LSDA 头不是 ff 9b"
    fi
    if readelf -SW "$obj" | grep -q 'rela\.gcc_except_table'; then
        bad "$label：.gcc_except_table 内部有重定位（catch-all 的 null TType 项必须无重定位）"
    else
        ok "$label：.gcc_except_table 内部无重定位"
    fi
    if readelf --debug-dump=frames "$obj" | grep -q '"zPLR"'; then
        ok "$label：CIE augmentation = zPLR（personality + LSDA 都在）"
    else
        bad "$label：CIE augmentation 不是 zPLR"
    fi
}

# run_suite <目录> <标签>：全量断言 + RSS 用例
run_suite() {
    local dir=$1 label=$2
    local out="$dir/out.txt" npass
    if ! timeout -s KILL 180 "$dir/z2_rt" > "$out" 2>&1; then
        bad "$label：测试程序退出码非 0"
        tail -25 "$out" | sed 's/^/    /'
        return 1
    fi
    npass=$(grep -c '^PASS: ' "$out")
    if [ "$npass" -eq 0 ] || ! grep -q '^SUMMARY: .* 0 failures' "$out"; then
        bad "$label：有断言失败"
        grep '^FAIL: \|^SUMMARY: ' "$out" | sed 's/^/    /'
        return 1
    fi
    CHECKS=$((CHECKS + npass))
    ok "$label：$npass 项断言全部通过（含 10 万次循环 RSS）"
    grep '^\[z2\]' "$out" | sed 's/^/    /'
}

# run_gc_off <目录> <标签>：GC 关闭 + 只跑 10 万次循环（V1 的强判据：不依赖 GC）
run_gc_off() {
    local dir=$1 label=$2
    local out="$dir/loop_gcoff.txt" npass delta
    if ! PYADAMAS_GC_DISABLE=1 Z2_ONLY_LOOP=1 timeout -s KILL 180 \
            "$dir/z2_rt" > "$out" 2>&1; then
        bad "$label：GC 关闭时 10 万次循环失败"
        tail -20 "$out" | sed 's/^/    /'
        return 1
    fi
    npass=$(grep -c '^PASS: ' "$out")
    delta=$(sed -n 's/^\[z2\] rss_delta_kb=//p' "$out" | head -1)
    if [ -z "$delta" ]; then
        bad "$label：没有拿到 RSS 增量"
        return 1
    fi
    if [ "$delta" -gt 1024 ]; then
        bad "$label：GC 关闭时 RSS 增量 $delta KB > 1024 KB（帧释放没做）"
        return 1
    fi
    CHECKS=$((CHECKS + npass))
    ok "$label：GC 关闭下 10 万次抛接 RSS 增量 $delta KB <= 1024 KB（$npass 项断言）"
}

# run_negative <目录> <标签>：反证「不压帧记录时必须失败」
run_negative() {
    local dir=$1 label=$2
    local out="$dir/neg.txt" out2="$dir/neg_rss.txt" rc delta
    PYADAMAS_GC_DISABLE=1 Z2_ONLY_LOOP=1 timeout -s KILL 180 \
        "$dir/z2_rt_neg" > "$out" 2>&1
    rc=$?
    if [ "$rc" -eq 0 ]; then
        bad "$label：反证程序竟然通过了（帧释放计数断言没抓住「没有记录可释放」）"
        tail -10 "$out" | sed 's/^/    /'
        return 1
    fi
    if ! grep -q '^FAIL: ' "$out"; then
        bad "$label：反证程序失败但没有帧释放相关的 FAIL 输出"
        return 1
    fi
    ok "$label：反证 1 通过 —— 不压帧记录时帧释放计数断言立刻失败（退出码 $rc）"
    grep '^FAIL: ' "$out" | head -3 | sed 's/^/    /'

    PYADAMAS_GC_DISABLE=1 Z2_ONLY_LOOP=1 Z2_NEG_KEEP_GOING=1 timeout -s KILL 300 \
        "$dir/z2_rt_neg" > "$out2" 2>&1
    rc=$?
    delta=$(sed -n 's/^\[z2\] rss_delta_kb=//p' "$out2" | head -1)
    if [ "$rc" -eq 0 ]; then
        bad "$label：RSS 反证程序竟然通过了"
    elif [ -z "$delta" ]; then
        bad "$label：RSS 反证没有拿到 RSS 增量"
    elif [ "$delta" -le 1024 ]; then
        bad "$label：泄漏变体只涨了 $delta KB <= 1024 KB，RSS 判据抓不住"
    else
        ok "$label：反证 2 通过 —— 泄漏变体 RSS 涨了 $delta KB（> 1024 KB，RSS 判据能抓住漏释放）"
    fi
}

# ── 3) -no-pie 一套 ─────────────────────────────────────────────────
CFLAGS_BASE="-O2 -g -Wall -Wextra -Werror -funwind-tables"
if build_suite "$BUILD/nopie" "$CFLAGS_BASE -fno-pie" "-no-pie"; then
    ok "夹具 + 运行时编译链接（-no-pie）"
    check_fixture_shape "$BUILD/nopie/z2_fixture.o" "-no-pie"
    run_suite "$BUILD/nopie" "-no-pie"
    run_gc_off "$BUILD/nopie" "-no-pie"
    run_negative "$BUILD/nopie" "-no-pie"
else
    bad "夹具 + 运行时编译链接失败（-no-pie）"
fi
echo

# ── 4) PIE 一套 ─────────────────────────────────────────────────────
if build_suite "$BUILD/pie" "$CFLAGS_BASE -fPIE" "-pie"; then
    ok "夹具 + 运行时编译链接（PIE）"
    check_fixture_shape "$BUILD/pie/z2_fixture.o" "PIE"
    run_suite "$BUILD/pie" "PIE"
    run_gc_off "$BUILD/pie" "PIE"
    run_negative "$BUILD/pie" "PIE"
else
    bad "夹具 + 运行时编译链接失败（PIE）"
fi
echo

# ── 5) 汇总 ─────────────────────────────────────────────────────────
if [ "$FAILS" -eq 0 ]; then
    echo "==== 结果：全部 PASS（两套链接共 $CHECKS 项断言；含 GC 开/关与两组反证）===="
    exit 0
fi
echo "==== 结果：$FAILS 项失败（共 $CHECKS 项断言通过）===="
exit 1
