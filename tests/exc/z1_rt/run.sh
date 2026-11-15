#!/bin/bash
# tests/exc/z1_rt/run.sh —— 一条命令验证「手写汇编 + personality + LSDA」端到端管线（Z1.5）
#
#   用法：bash tests/exc/z1_rt/run.sh
#   退出码：0 = 全部通过；1 = 有失败
#
# 验证内容（全部用 gcc/g++ 直接编译，不碰仓库的 CMake 构建）：
#   1. runtime/rt_personality.c 能通过 -O2 -Wall -Wextra -Werror -I runtime 严格编译；
#   2. 6 份手写汇编夹具 + driver 编译链接成测试程序；
#   3. -no-pie 和 PIE 两种链接方式下 driver 断言全部通过：
#      第一步（无 LSDA 的 plumbing + 0x1b 直连 personality）、
#      第二步（GCC 同款 20 字节 LSDA：ttype=0x9b + null 项 + filter=1 catch-all）、
#      负向 A（TType 编码非法）、负向 B（call-site 表越界）、
#      filter<0（exception spec）的 CONTINUE 选择、
#      FORCE_UNWIND 下不调用 _Unwind_SetIP、
#      第三步（两阶段展开顺序、无 landing pad 帧被穿过、_Unwind_Resume）；
#   4. 交叉验证：同一份 z1_step2.S 用 -Wa,--defsym,Z1_CROSSCHECK=1 编出
#      「保护区只有一条 C++ throw」的变体，personality 槽换成真的
#      __gxx_personality_v0，由 C++ 外层 try/catch(int) 观察 —— 夹具帧必须被
#      当作 catch-all（LP 跑 1 次、异常被吃掉、caught=0），两套链接都跑。
#
# 依赖：Linux x86-64、gcc/g++、binutils（readelf）、libstdc++/libgcc_s。
# 构建产物放临时目录，退出时删除。

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
CC=${CC:-gcc}
CXX=${CXX:-g++}
BUILD=$(mktemp -d "${TMPDIR:-/tmp}/z1_rt.XXXXXX") || { echo "mktemp 失败"; exit 1; }
trap 'rm -rf "$BUILD"' EXIT

FAILS=0
CHECKS=0
FIXTURES="z1_step1 z1_step2 z1_step3 z1_broken_lsda z1_broken_table z1_spec_lsda"

ok()  { printf '[PASS] %s\n' "$*"; }
bad() { printf '[FAIL] %s\n' "$*"; FAILS=$((FAILS + 1)); }

echo "==== z1_rt（Z1.5：ABI 语义 + GCC 同款 LSDA）===="
echo "仓库根：$ROOT"
echo "构建目录：$BUILD"
echo

# ── 1) 验收条件：rt_personality.c 严格编译（将来直接进 rt.o） ─────────
if "$CC" -O2 -Wall -Wextra -Werror -I "$ROOT/runtime" \
        -c "$ROOT/runtime/rt_personality.c" -o "$BUILD/rt_personality_strict.o"; then
    ok "rt_personality.c 严格编译（-O2 -Wall -Wextra -Werror -I runtime）"
else
    bad "rt_personality.c 严格编译失败"
fi

# ── 2) 测试程序：build_suite <目录> <CFLAGS 后缀> <链接参数> ─────────
build_suite() {
    local dir=$1 cflags=$2 lflags=$3
    mkdir -p "$dir" || return 1
    "$CC" $cflags -DPA_PERSONALITY_TRACE -I "$ROOT/runtime" \
        -c "$ROOT/runtime/rt_personality.c" -o "$dir/rt_personality.o" || return 1
    "$CC" $cflags -DPA_PERSONALITY_TRACE -c "$HERE/driver.c" -o "$dir/driver.o" || return 1
    "$CC" $cflags -c "$HERE/stub_personality.c" -o "$dir/stub_personality.o" || return 1
    local f objlist=""
    for f in $FIXTURES; do
        "$CC" -c "$HERE/$f.S" -o "$dir/$f.o" || return 1
        objlist="$objlist $dir/$f.o"
    done
    "$CC" $lflags -o "$dir/z1_rt" "$dir/driver.o" "$dir/stub_personality.o" \
        "$dir/rt_personality.o" $objlist || return 1
}

# run_suite <目录> <标签>：跑 driver，断言全通过，打印 driver 的 PASS/SUMMARY
run_suite() {
    local dir=$1 label=$2
    if ! timeout -s KILL 30 "$dir/z1_rt" > "$dir/out.txt" 2> "$dir/err.txt"; then
        bad "$label：测试程序退出码非 0"
        sed 's/^/    /' "$dir/err.txt" | head -20
        grep '^PASS: \|^FAIL: \|^SUMMARY: ' "$dir/out.txt" | sed 's/^/    /'
        return 1
    fi
    local npass
    npass=$(grep -c '^PASS: ' "$dir/out.txt")
    if grep -q '^SUMMARY: .* 0 failures' "$dir/out.txt" && [ "$npass" -gt 0 ]; then
        CHECKS=$((CHECKS + npass))
        ok "$label：$npass 项断言全部通过"
        grep '^PASS: \|^SUMMARY: ' "$dir/out.txt" | sed 's/^/    /'
    else
        bad "$label：有断言失败"
        grep '^FAIL: \|^SUMMARY: ' "$dir/out.txt" | sed 's/^/    /'
        return 1
    fi
}

# check_lsda_shape <对象文件> <标签>：确认 GCC 同款 LSDA 形态
check_lsda_shape() {
    local obj=$1 label=$2
    if readelf -x .gcc_except_table "$obj" | grep -q 'ff9b'; then
        ok "$label：LSDA 头为 ff 9b ...（TType=0x9b，GCC 同款）"
    else
        bad "$label：LSDA 头不是 ff 9b ..."
    fi
    if readelf -SW "$obj" | grep -q 'rela\.gcc_except_table'; then
        bad "$label：.gcc_except_table 内部有重定位（catch-all 的 null TType 项必须无重定位）"
    else
        ok "$label：.gcc_except_table 内部无重定位（null TType 项符合 GCC 写法）"
    fi
}

# ── 3) -no-pie 一套 ─────────────────────────────────────────────────
CFLAGS_BASE="-O2 -g -Wall -Wextra -Werror -funwind-tables"
if build_suite "$BUILD/nopie" "$CFLAGS_BASE -fno-pie" "-no-pie"; then
    ok "夹具编译 + 链接（-no-pie）"
    check_lsda_shape "$BUILD/nopie/z1_step2.o" "z1_step2（-no-pie）"
    check_lsda_shape "$BUILD/nopie/z1_step3.o" "z1_step3（-no-pie）"
    if readelf --debug-dump=frames "$BUILD/nopie/z1_step1.o" | grep -q '"zPR"'; then
        ok "z1_step1.o 的 CIE augmentation = \"zPR\"（0x1b 直连 personality、无 LSDA）"
    else
        bad "z1_step1.o 的 CIE augmentation 不对"
    fi
    if readelf --debug-dump=frames "$BUILD/nopie/z1_step2.o" | grep -qE 'Augmentation data: *9b'; then
        ok "z1_step2.o 的 CIE 用 0x9b 间接 personality（gcc 同款编码）"
    else
        bad "z1_step2.o 的 CIE personality 编码不是 0x9b"
    fi
    run_suite "$BUILD/nopie" "-no-pie"
else
    bad "夹具编译 + 链接（-no-pie）"
fi
echo

# ── 4) PIE 一套（同断言，验证 pcrel/0x9b 槽在 PIE 下也成立） ────────
if build_suite "$BUILD/pie" "$CFLAGS_BASE -fPIE" "-pie"; then
    ok "夹具编译 + 链接（PIE）"
    run_suite "$BUILD/pie" "PIE"
else
    bad "夹具编译 + 链接（PIE）"
fi
echo

# ── 5) 真 __gxx_personality_v0 交叉验证 ─────────────────────────────
# run_crosscheck <目录> <CFLAGS 后缀> <链接参数> <标签>
run_crosscheck() {
    local dir=$1 cflags=$2 lflags=$3 label=$4
    mkdir -p "$dir" || return 1
    "$CC" -c -Wa,--defsym,Z1_CROSSCHECK=1 "$HERE/z1_step2.S" \
        -o "$dir/z1_step2_cc.o" || return 1
    "$CXX" $cflags -c "$HERE/crosscheck_gxx.cpp" -o "$dir/crosscheck.o" || return 1
    "$CXX" $lflags -o "$dir/crosscheck" "$dir/crosscheck.o" "$dir/z1_step2_cc.o" || return 1
    local out rc
    out=$(timeout -s KILL 20 "$dir/crosscheck" 2>&1)
    rc=$?
    if [ "$rc" -eq 0 ] && printf '%s' "$out" | grep -q '^\[crosscheck\] caught=0 lp_calls=1 fn_ret=7 z1_lp_seen=1$'; then
        ok "$label：真 __gxx_personality_v0 把夹具帧当 catch-all（LP 1 次、异常被吃掉）"
        printf '%s\n' "$out" | sed 's/^/    /'
    else
        bad "$label：真 gcc personality 交叉验证失败（rc=$rc）"
        printf '%s\n' "$out" | sed 's/^/    /'
        return 1
    fi
}

if command -v "$CXX" >/dev/null 2>&1; then
    if run_crosscheck "$BUILD/cc_nopie" "$CFLAGS_BASE -fno-pie" "-no-pie" "交叉验证（-no-pie）"; then :; fi
    echo
    if run_crosscheck "$BUILD/cc_pie" "$CFLAGS_BASE -fPIE" "-pie" "交叉验证（PIE）"; then :; fi
else
    bad "找不到 g++，无法做真 __gxx_personality_v0 交叉验证"
fi
echo

# ── 6) 汇总 ─────────────────────────────────────────────────────────
if [ "$FAILS" -eq 0 ]; then
    echo "==== 结果：全部 PASS（两套链接共 $CHECKS 项断言）===="
    exit 0
fi
echo "==== 结果：$FAILS 项失败（共 $CHECKS 项断言通过）===="
exit 1
