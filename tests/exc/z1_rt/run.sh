#!/bin/bash
# tests/exc/z1_rt/run.sh —— 一条命令验证「手写汇编 + personality + LSDA」端到端管线
#
#   用法：bash tests/exc/z1_rt/run.sh
#   退出码：0 = 全部通过；1 = 有失败
#
# 验证内容（全部用 gcc 直接编译，不碰仓库的 CMake 构建）：
#   1. runtime/rt_personality.c 能通过 -O2 -Wall -Wextra -Werror -I runtime 严格编译；
#   2. 四份手写汇编夹具 + driver 能编译链接成测试程序；
#   3. -no-pie 和 PIE 两种链接方式下，driver 里的 39 项断言全部通过：
#      第一步（无 LSDA 的 plumbing）、第二步（真 LSDA 解码 + landing pad）、
#      负向用例（故意改坏 LSDA 一个字节）、第三步（两阶段展开 cleanup/handler 顺序、
#      没有 landing pad 的中间帧被静默穿过、cleanup landing pad 里 _Unwind_Resume）。
#
# 依赖：Linux x86-64、gcc、binutils（readelf）、libgcc_s。构建产物放临时目录，退出时删除。

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
CC=${CC:-gcc}
BUILD=$(mktemp -d "${TMPDIR:-/tmp}/z1_rt.XXXXXX") || { echo "mktemp 失败"; exit 1; }
trap 'rm -rf "$BUILD"' EXIT

FAILS=0
CHECKS=0

ok()  { printf '[PASS] %s\n' "$*"; }
bad() { printf '[FAIL] %s\n' "$*"; FAILS=$((FAILS + 1)); }

echo "==== z1_rt：手写汇编 + pa_personality 端到端验证 ===="
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

# ── 2) 构建测试程序：build_suite <目录> <CFLAGS 后缀> <链接参数> ─────
build_suite() {
    local dir=$1 cflags=$2 lflags=$3
    mkdir -p "$dir" || return 1
    "$CC" $cflags -DPA_PERSONALITY_TRACE -I "$ROOT/runtime" \
        -c "$ROOT/runtime/rt_personality.c" -o "$dir/rt_personality.o" || return 1
    "$CC" $cflags -DPA_PERSONALITY_TRACE -c "$HERE/driver.c" -o "$dir/driver.o" || return 1
    "$CC" $cflags -c "$HERE/stub_personality.c" -o "$dir/stub_personality.o" || return 1
    local f
    for f in z1_step1 z1_step2 z1_step3 z1_broken_lsda; do
        "$CC" -c "$HERE/$f.S" -o "$dir/$f.o" || return 1
    done
    "$CC" $lflags -o "$dir/z1_rt" "$dir/driver.o" "$dir/stub_personality.o" \
        "$dir/rt_personality.o" "$dir/z1_step1.o" "$dir/z1_step2.o" \
        "$dir/z1_step3.o" "$dir/z1_broken_lsda.o" || return 1
}

# run_suite <目录> <标签>：跑 driver，断言全通过，打印 driver 的 PASS/SUMMARY
run_suite() {
    local dir=$1 label=$2
    if ! timeout -s KILL 30 "$dir/z1_rt" > "$dir/out.txt" 2> "$dir/err.txt"; then
        bad "$label：测试程序退出码非 0（stderr 见下）"
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

# ── 3) -no-pie 一套 ─────────────────────────────────────────────────
CFLAGS_BASE="-O2 -g -Wall -Wextra -Werror -funwind-tables"
if build_suite "$BUILD/nopie" "$CFLAGS_BASE -fno-pie" "-no-pie"; then
    ok "夹具编译 + 链接（-no-pie）"
    # 结构自检：LSDA 与 .eh_frame 的 personality/LSDA 字段确实emit出来了
    if readelf -S "$BUILD/nopie/z1_step2.o" | grep -q '\.gcc_except_table'; then
        ok "z1_step2.o 生成 .gcc_except_table 节"
    else
        bad "z1_step2.o 没有 .gcc_except_table"
    fi
    if readelf --debug-dump=frames "$BUILD/nopie/z1_step2.o" | grep -q '"zPLR"'; then
        ok "z1_step2.o 的 CIE augmentation = \"zPLR\"（personality + LSDA + FDE 编码齐全）"
    else
        bad "z1_step2.o 的 CIE 缺少 personality/LSDA augmentation"
    fi
    if readelf --debug-dump=frames "$BUILD/nopie/z1_step1.o" | grep -q '"zPR"'; then
        ok "z1_step1.o 的 CIE augmentation = \"zPR\"（只有 personality，没有 LSDA）"
    else
        bad "z1_step1.o 的 CIE augmentation 不对"
    fi
    run_suite "$BUILD/nopie" "-no-pie"
else
    bad "夹具编译 + 链接（-no-pie）"
fi
echo

# ── 4) PIE 一套（验证 .cfi_personality/.cfi_lsda 的 pcrel 编码在 PIE 下也成立） ──
if build_suite "$BUILD/pie" "$CFLAGS_BASE -fPIE" "-pie"; then
    ok "夹具编译 + 链接（PIE）"
    run_suite "$BUILD/pie" "PIE"
else
    bad "夹具编译 + 链接（PIE）"
fi
echo

# ── 5) 汇总 ─────────────────────────────────────────────────────────
if [ "$FAILS" -eq 0 ]; then
    echo "==== 结果：全部 PASS（两套链接共 $CHECKS 项断言）===="
    exit 0
fi
echo "==== 结果：$FAILS 项失败（共 $CHECKS 项断言通过）===="
exit 1
