#!/bin/bash
# tests/eh/run.sh —— LSDA 发射器的 golden 字节测试
#
#   用法：bash tests/eh/run.sh
#   退出码：0 = 全部通过；1 = 有失败
#
# 验证内容（全部用 gcc/g++ 直接编译，不碰仓库的 CMake 构建）：
#   1. src/backend/eh_emit.cpp 能通过 -std=c++20 -Wall -Wextra -Werror 严格编译；
#   2. 发射器为 6 个用例生成 .s（含 eh_plan_check 自检）；
#   3. gcc -c 汇编后，用 objcopy 抽出 .gcc_except_table 原始字节，与
#      tools/lsda_ref/FINDINGS.md §0.1 / §3 里 gcc 14.2 实测的基准逐字节比对；
#   4. 结构断言：CIE augmentation 必须 zPLR 且 personality 编码是 0x9b（间接）、
#      DW.ref 槽上有 pa_personality 重定位、.gcc_except_table 上不能有重定位
#      （TType 的 null 项必须是裸 0）。
#
# 依赖：gcc/g++、binutils（readelf/objcopy/od）。构建产物放 mktemp，退出即删。

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
CXX=${CXX:-g++}
CC=${CC:-gcc}
BUILD=$(mktemp -d "${TMPDIR:-/tmp}/eh_emit.XXXXXX") || { echo "mktemp 失败"; exit 1; }
trap 'rm -rf "$BUILD"' EXIT

FAILS=0
CHECKS=0

ok()  { printf '[PASS] %s\n' "$*"; CHECKS=$((CHECKS + 1)); }
bad() { printf '[FAIL] %s\n' "$*"; FAILS=$((FAILS + 1)); }

echo "==== tests/eh：LSDA 发射器 golden 字节测试 ===="
echo "仓库根：$ROOT"
echo "构建目录：$BUILD"
echo

# ── 1) 构建生成器（严格告警，与仓库构建口径一致） ─────────────────────
if ! "$CXX" -std=c++20 -O1 -Wall -Wextra -Werror -I "$ROOT/src" \
        "$ROOT/tests/eh/main.cpp" "$ROOT/src/backend/eh_emit.cpp" -o "$BUILD/eh_gen"; then
    bad "编译 src/backend/eh_emit.cpp + tests/eh/main.cpp 失败"
    echo "==== 结果：FAIL ===="
    exit 1
fi
ok "编译 src/backend/eh_emit.cpp + tests/eh/main.cpp（-std=c++20 -Wall -Wextra -Werror）"

mkdir -p "$BUILD/gen" || exit 1
if ! "$BUILD/eh_gen" "$BUILD/gen" > "$BUILD/gen.log" 2>&1; then
    bad "测试驱动生成 .s 失败"
    sed 's/^/    /' "$BUILD/gen.log"
    echo "==== 结果：FAIL ===="
    exit 1
fi
ok "生成 6 份 .s（含 eh_plan_check / 非法计划抛异常自检）"
sed 's/^/    /' "$BUILD/gen.log"

# 期望字节写死在这里：来源是 gcc 14.2 实测（tools/lsda_ref/FINDINGS.md
# §0.1 的 20 字节基准、§3.3 的 12 字节 cleanup 基准）。用例 1/4 没有
# 现成的 gcc 产物，期望值是按 §0.1 的字段口径手算的：
#   - catch_single：header 5 + call-site 4 + action 2 + padding 1 + TType 4 = 16；
#   - multi_handler：action 字段是 action 表内 1 起的**字节偏移**（1 和 3），
#     记录各 2 字节；header 5 + call-site 14（含 2 字节 uleb 偏移）+ action 4
#     + padding 1 + TType 4 = 28；ttype_offset = 28 - 3 = 0x19。
declare -A EXPECT=(
    [catch_single]="ff9b0d01040405130101000000000000"
    [catch_gcc20]="ff9b110108040513011b05000001000000000000"
    [cleanup_gcc12]="ffff010805050c001c050000"
    [multi_handler]="ff9b19010e040513010a050b00a00205b00203010001000000000000"
    [catch_gcc20_labels]="ff9b110108040513011b05000001000000000000"
    # 两个单 handler 计划（各 16 字节）拼在同一 .gcc_except_table 段里
    [combine_two]="ff9b0d01040405130101000000000000ff9b0d01040803200101000000000000"
)

# ── 2) 逐用例：汇编 → 抽 .gcc_except_table → 逐字节比对 + 结构断言 ──────
for name in catch_single catch_gcc20 cleanup_gcc12 multi_handler catch_gcc20_labels combine_two; do
    src="$BUILD/gen/$name.s"
    obj="$BUILD/$name.o"
    bin="$BUILD/$name.bin"

    if ! "$CC" -c "$src" -o "$obj" > "$BUILD/$name.cc.log" 2>&1; then
        bad "$name：汇编失败"
        sed 's/^/    /' "$BUILD/$name.cc.log"
        continue
    fi
    if ! objcopy -O binary --only-section=.gcc_except_table "$obj" "$bin" \
            2> "$BUILD/$name.objcopy.log"; then
        bad "$name：objcopy 抽取 .gcc_except_table 失败"
        sed 's/^/    /' "$BUILD/$name.objcopy.log"
        continue
    fi
    actual=$(od -An -v -tx1 "$bin" | tr -d ' \n')
    expected=${EXPECT[$name]}
    if [ "$actual" = "$expected" ]; then
        ok "$name：逐字节一致（$(( ${#actual} / 2 )) 字节）"
        printf '    实际：%s\n    期望：%s\n' "$actual" "$expected"
    else
        bad "$name：字节不一致"
        printf '    实际：%s\n    期望：%s\n' "$actual" "$expected"
    fi

    # 结构断言 1：CIE augmentation 必须是 zPLR（personality + LSDA 都在）。
    if readelf -wf "$obj" | grep -q 'zPLR'; then
        ok "$name：.eh_frame CIE augmentation = zPLR"
    else
        bad "$name：.eh_frame 里没有 zPLR（personality/LSDA 少了一个）"
    fi
    # 结构断言 2：personality 指针编码必须是 0x9b（间接），0x9b 后面必须走
    # DW.ref 槽 —— 直接写函数名会汇编通过、运行期 SIGSEGV。
    if readelf -wf "$obj" | grep -qE 'Augmentation data: +9b '; then
        ok "$name：personality 编码 = 0x9b（indirect）"
    else
        bad "$name：personality 编码不是 0x9b"
    fi
    if readelf -rW "$obj" | grep -q 'pa_personality'; then
        ok "$name：DW.ref 槽有 pa_personality 重定位"
    else
        bad "$name：DW.ref 槽没有指向 pa_personality 的重定位"
    fi
    # 结构断言 3：.gcc_except_table 上不能有任何重定位（TType 的 null 项
    # 必须是裸 4 字节 0，带重定位就不是 catch(...) 了）。
    if readelf -SW "$obj" | grep -q '\.rela\.gcc_except_table'; then
        bad "$name：.gcc_except_table 上出现了重定位（TType 表项必须是裸 0）"
    else
        ok "$name：.gcc_except_table 无重定位"
    fi
done

# ── 3) combine_two：DW.ref 槽只能有一个（每个 TU 一次） ──────────────
if [ -f "$BUILD/combine_two.o" ]; then
    n_dwref=$(readelf -sW "$BUILD/combine_two.o" | grep -c 'DW.ref.pa_personality')
    if [ "$n_dwref" -eq 1 ]; then
        ok "combine_two：TU 里只有一个 DW.ref.pa_personality 槽"
    else
        bad "combine_two：DW.ref.pa_personality 槽出现 $n_dwref 次（应为 1）"
    fi
fi

echo
if [ "$FAILS" -eq 0 ]; then
    echo "==== 结果：全部 PASS（$CHECKS 项断言）===="
    exit 0
fi
echo "==== 结果：$FAILS 项 FAIL（共 $((CHECKS + FAILS)) 项）===="
exit 1
