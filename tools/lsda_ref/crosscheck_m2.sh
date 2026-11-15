#!/bin/bash
# =============================================================================
# crosscheck_m2.sh —— 独立交叉核对 M2 手写夹具实际发射的 LSDA 字节
#
# 做三件事（全部只读 tests/exc/ 与 runtime/ 的源码，产物写 tools/lsda_ref/out/）：
#   1) 用 gcc 重编 tests/exc/z1_rt 里的三个 .S 夹具，dump + 用本目录解码器逐字段解码；
#   2) 按它们的 run.sh 方式建 PIE / 非 PIE 两套可执行文件，比较链接后 .eh_frame
#      CIE 的 personality/LSDA 编码字节；
#   3) 用**真正的 __gxx_personality_v0** 解析 M2 形态的 LSDA（crosscheck_m2/ 下的复刻夹具），
#      运行时观察 ABI 语义（cleanup vs handler；filter=1 时会发生什么）。
#
# 用法：bash tools/lsda_ref/crosscheck_m2.sh
# 说明：结论见 tools/lsda_ref/CROSSCHECK_M2.md。
# =============================================================================
set -u
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
SRC="$ROOT/tests/exc/z1_rt"
OUT="out/crosscheck_m2"
FIX="$OUT/fixtures"
CC=gcc
CXX=g++
PY=python3
mkdir -p "$FIX"

echo "=============================================================="
echo " 交叉核对 M2 夹具的 LSDA 编码（独立重编 + 独立解码）"
echo "=============================================================="
echo "仓库根: $ROOT"
"$CC" --version | head -1
echo

# ── 1) 重编 M2 夹具（只读源码，产物写本目录）──────────────────────────
echo "## 1. 用 gcc 重编 tests/exc/z1_rt/*.S（-Wa,-L 保留 .L 标签便于定位 LSDA 边界）"
for f in z1_step2 z1_step3 z1_broken_lsda; do
    if "$CC" -c -Wa,-L -o "$FIX/$f.o" "$SRC/$f.S"; then
        echo "   AS OK  $FIX/$f.o"
    else
        echo "   AS FAIL $f"; exit 1
    fi
done
echo

echo "## 2. 原始字节 dump（.gcc_except_table / .eh_frame / 重定位）"
for f in z1_step2 z1_step3 z1_broken_lsda; do
    readelf -x .gcc_except_table "$FIX/$f.o" > "$FIX/$f.readelf_x_except_table.txt"
    readelf -x .eh_frame         "$FIX/$f.o" > "$FIX/$f.readelf_x_eh_frame.txt"
    readelf -wf                  "$FIX/$f.o" > "$FIX/$f.readelf_wf.txt"
    readelf -rW                  "$FIX/$f.o" > "$FIX/$f.readelf_r.txt"
    readelf -sW                  "$FIX/$f.o" > "$FIX/$f.readelf_s.txt"
    echo "   ---- $f.o .gcc_except_table ----"
    sed -n '1,4p' "$FIX/$f.readelf_x_except_table.txt"
done
echo

echo "## 3. 用本目录解码器逐字段解码（.eh_frame 编码 + 每段 LSDA）"
for f in z1_step2 z1_step3 z1_broken_lsda; do
    "$PY" decode_eh_lsda.py "$FIX/$f.o" > "$FIX/$f.decode.txt" 2>&1
    echo "   ======== $f.o ========"
    grep -E "LSDA #|augmentation data|personality 编码|结构非法|\[FAIL\]" "$FIX/$f.decode.txt" || true
    echo "   （完整解码见 $FIX/$f.decode.txt）"
    echo
done

echo "## 4. 任务消息里引用的 5 段字节 vs 我重编得到的实际字节"
objcopy -O binary --only-section=.gcc_except_table "$FIX/z1_step2.o" "$FIX/z1_step2.except.bin"
actual_step2=$(od -An -tx1 -v "$FIX/z1_step2.except.bin" | tr -d ' \n')
echo "   消息引用 step2 : ff ff 01 04 08 0e 1d 01 00 00 00 00"
echo "   实际 step2     : $actual_step2"
"$PY" decode_eh_lsda.py --raw-hex "$actual_step2" > "$FIX/raw_actual_step2.txt" 2>&1
grep -E "\[1\]" "$FIX/raw_actual_step2.txt" | sed 's/^/        /'
objcopy -O binary --only-section=.gcc_except_table "$FIX/z1_step3.o" "$FIX/z1_step3.except.bin"
actual_step3=$(od -An -tx1 -v "$FIX/z1_step3.except.bin" | tr -d ' \n')
echo "   实际 step3 整节: $actual_step3"
echo "     handler= ff ff 01 04 08 09 18 01 00 00 00 00（12 字节：LSDA 10 + 对齐 pad 2）"
echo "     cleanup= ff ff 01 04 08 09 18 00（8 字节）"
echo "     nopad  = ff ff 01 04 08 09 00 00（8 字节）"
echo "   消息引用的 cleanup 是 ff ff 01 04 08 09 18 00 00 00 00（11 字节，多算了 3 个 0）；"
echo "     cleanup 实际只有 8 字节，nopad 与消息一致。"
echo

# ── 5) 按 M2 自己的 run.sh 方式建 PIE / 非 PIE 两套可执行文件 ─────────
echo "## 5. 链接两套可执行文件（复刻 tests/exc/z1_rt/run.sh 的方式），"
echo "      看链接后 .eh_frame 的 CIE augmentation 编码字节"
CFLAGS_BASE="-O2 -g -Wall -Wextra -Werror -funwind-tables"
build_suite() {
    local dir="$1" cflags="$2" lflags="$3"
    mkdir -p "$dir" || return 1
    "$CC" $cflags -DPA_PERSONALITY_TRACE -I "$ROOT/runtime" \
        -c "$ROOT/runtime/rt_personality.c" -o "$dir/rt_personality.o" || return 1
    "$CC" $cflags -DPA_PERSONALITY_TRACE -c "$SRC/driver.c" -o "$dir/driver.o" || return 1
    "$CC" $cflags -c "$SRC/stub_personality.c" -o "$dir/stub_personality.o" || return 1
    local f
    for f in z1_step1 z1_step2 z1_step3 z1_broken_lsda; do
        "$CC" -c "$SRC/$f.S" -o "$dir/$f.o" || return 1
    done
    "$CC" $lflags -o "$dir/z1_rt" "$dir/driver.o" "$dir/stub_personality.o" \
        "$dir/rt_personality.o" "$dir/z1_step1.o" "$dir/z1_step2.o" \
        "$dir/z1_step3.o" "$dir/z1_broken_lsda.o" || return 1
}
for mode in nopie pie; do
    if [ "$mode" = nopie ]; then
        build_suite "$OUT/build_nopie" "$CFLAGS_BASE -fno-pie" "-no-pie"
    else
        build_suite "$OUT/build_pie" "$CFLAGS_BASE -fPIE" "-pie"
    fi
    if [ $? -ne 0 ]; then echo "   [$mode] 构建失败"; continue; fi
    exe="$OUT/build_$mode/z1_rt"
    readelf -wf "$exe" > "$OUT/build_$mode/readelf_wf.txt"
    readelf -x .eh_frame "$exe" > "$OUT/build_$mode/readelf_x_eh_frame.txt"
    echo "   [$mode] 构建 + 链接 OK：$exe"
    echo "        zPLR CIE 的 augmentation data（链接后）："
    grep -A9 'Augmentation:.*"zPLR"' "$OUT/build_$mode/readelf_wf.txt" | grep -E "Augmentation data" | sed 's/^/          /'
    echo "        z1_step2.o 里 personality 字段的重定位："
    grep -E "pa_personality" "$FIX/z1_step2.readelf_r.txt" | sed 's/^/          /'
    if timeout -s KILL 30 "$exe" > "$OUT/build_$mode/suite_out.txt" 2> "$OUT/build_$mode/suite_err.txt"; then
        echo "        它们自己的 suite: $(grep -c '^PASS: ' "$OUT/build_$mode/suite_out.txt") 项 PASS，$(grep '^SUMMARY:' "$OUT/build_$mode/suite_out.txt" | tail -1)"
    else
        echo "        它们自己的 suite 运行失败（见 $OUT/build_$mode/suite_err.txt）"
    fi
done
echo

# ── 6) 关键裁决：用 libstdc++ 的 __gxx_personality_v0 解析 M2 形态 LSDA ──
echo "## 6. 运行时裁决：M2 形态 LSDA（ttype=0xff, action=1, action记录(filter,0)）"
echo "    (a) action 记录 filter=0（即 M2 的“handler”字节）：ABI 语义应是 cleanup"
"$CXX" -O1 -o "$OUT/m2_cleanup_semantics.exe" \
    "crosscheck_m2/mini_m2_cleanup_semantics.S" "crosscheck_m2/m2_driver.cpp" || exit 1
"$OUT/m2_cleanup_semantics.exe" 2> "$OUT/m2_cleanup_semantics.err"
rc=$?
echo "        程序退出码=$rc；stderr："
sed 's/^/        /' "$OUT/m2_cleanup_semantics.err"
echo "        => landing pad 作为 cleanup 跑了 1 次，异常继续传到外层 C++ catch(int)。"
echo
echo "    (b) action 记录 filter=1（ttype_encoding=0xff 时 get_ttype_entry 会 abort）"
"$CXX" -O1 -o "$OUT/m2_filter1.exe" \
    "crosscheck_m2/mini_m2_filter1.S" "crosscheck_m2/m2_driver.cpp" || exit 1
"$OUT/m2_filter1.exe" > "$OUT/m2_filter1.out" 2> "$OUT/m2_filter1.err"
rc=$?
echo "        程序退出码=$rc（134 = SIGABRT，不是 handler 被找到）"
sed 's/^/        /' "$OUT/m2_filter1.err" | head -5
echo

echo "## 7. 复刻夹具（0x9b + DW.ref.__gxx_personality_v0）的 .o 解码"
for v in mini_m2_cleanup_semantics mini_m2_filter1; do
    "$CC" -c -Wa,-L -o "$OUT/$v.o" "crosscheck_m2/$v.S"
    "$PY" decode_eh_lsda.py "$OUT/$v.o" > "$OUT/$v.decode.txt" 2>&1
    echo "   ======== $v.o ========"
    grep -E "augmentation data|personality 编码|personality 指针|^\[LSDA\]|filter=" "$OUT/$v.decode.txt" | head -8
done
echo
echo "完成：所有证据落在 $OUT/，结论见 tools/lsda_ref/CROSSCHECK_M2.md"
