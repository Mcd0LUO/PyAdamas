#!/bin/bash
# =============================================================================
# tools/lsda_ref/gen.sh —— M1「LSDA 字节级 ground truth」一键重建脚本
#
# 作用：用 g++ 重新生成参考用例与手写 mini 汇编用例，dump 所有相关节，
#       并打印 .eh_frame / LSDA 的逐字段解码结果。
# 用法：bash tools/lsda_ref/gen.sh
# 产物：全部落在 tools/lsda_ref/out/（可随时整目录删掉重建）
# 注意：本脚本只调用 g++/readelf/objdump/objcopy/python3，不碰顶层构建系统
#       （不跑 cmake/ninja，不写 build/）。
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")"

OUT=out
CXX=${CXX:-g++}
READELF=${READELF:-readelf}
OBJDUMP=${OBJDUMP:-objdump}
OBJCOPY=${OBJCOPY:-objcopy}
PYTHON=${PYTHON:-python3}

need() { command -v "$1" >/dev/null 2>&1 || { echo "缺少工具: $1" >&2; exit 1; }; }
for t in "$CXX" "$READELF" "$OBJDUMP" "$OBJCOPY" "$PYTHON"; do need "$t"; done
mkdir -p "$OUT"

PROBES="probe_catch_int probe_catch_all probe_cleanup"
MINIS="mini_indirect mini_direct mini_direct_xo mini_direct_ext"

echo "=============================================================="
echo " M1 LSDA 字节级 ground truth（gcc 实测，可重复生成）"
echo "=============================================================="
echo
echo "## 0. 环境（判定结论以本机版本为准）"
"$CXX" --version | head -1
"$READELF" --version | head -1
"$OBJDUMP" --version | head -1
"$PYTHON" --version

echo
echo "## 1. 参考用例：g++ -O1 -S 与 g++ -O1 -c"
echo "   （-Wa,-L 让汇编器保留 .L 局部符号，便于把 LSDA 里的偏移对回 .LEHB/.LEHE 地址）"
for f in $PROBES; do
  "$CXX" -O1 -S -o "$OUT/$f.O1.s" "$f.cpp"
  "$CXX" -O1 -c -Wa,-L -o "$OUT/$f.O1.o" "$f.cpp"
  echo "   - $OUT/$f.O1.s  $OUT/$f.O1.o"
done

echo
echo "## 2. dump 各节（原始字节 + 重定位 + 符号表，全部落盘）"
for f in $PROBES; do
  "$OBJDUMP" -s -j .gcc_except_table "$OUT/$f.O1.o" > "$OUT/$f.objdump_s_except_table.txt"
  "$READELF" -x .gcc_except_table            "$OUT/$f.O1.o" > "$OUT/$f.readelf_x_except_table.txt"
  "$READELF" -wf                             "$OUT/$f.O1.o" > "$OUT/$f.readelf_wf.txt"
  "$OBJDUMP" -dr -j .text                    "$OUT/$f.O1.o" > "$OUT/$f.objdump_dr_text.txt"
  "$READELF" -rW                             "$OUT/$f.O1.o" > "$OUT/$f.readelf_r.txt"
  "$READELF" -sW                             "$OUT/$f.O1.o" > "$OUT/$f.readelf_s.txt"
  "$READELF" -SW                             "$OUT/$f.O1.o" > "$OUT/$f.readelf_S.txt"
  echo "   - $f.{objdump_s_except_table,readelf_x_except_table,readelf_wf,objdump_dr_text,readelf_r,readelf_s,readelf_S}.txt"
done

echo
echo "## 3. 链接成可执行文件：确认重定位应用后的 .gcc_except_table 字节"
for f in $PROBES; do
  "$CXX" -O1 -o "$OUT/$f.exe" "$OUT/$f.O1.o" probe_main.cpp
  "$READELF" -x .gcc_except_table "$OUT/$f.exe" > "$OUT/$f.exe_readelf_x_except_table.txt"
  "$READELF" -wf "$OUT/$f.exe" > "$OUT/$f.exe_readelf_wf.txt"
  "$OBJCOPY" -O binary --only-section=.gcc_except_table "$OUT/$f.O1.o" "$OUT/$f.obj.bin"
  "$OBJCOPY" -O binary --only-section=.gcc_except_table "$OUT/$f.exe"   "$OUT/$f.exe.bin"
  echo "   - $OUT/$f.exe（.o 与 .exe 字节对比见第 6 节）"
done

echo
echo "## 4. 逐字段解码 .eh_frame 编码与 LSDA（人类可读，落盘 $OUT/<probe>.decode.txt）"
for f in $PROBES; do
  "$PYTHON" decode_eh_lsda.py "$OUT/$f.O1.o" | tee "$OUT/$f.decode.txt"
  echo
done

echo "## 5. 手写汇编 mini 用例：汇编 + 链接 + dump + 解码"
build_mini() {
  local name="$1" src="$2"
  "$CXX" -c -Wa,-L -o "$OUT/$name.o" "$src"
  echo "   - 汇编通过: $OUT/$name.o  ($src)"
}
build_mini mini_indirect   mini.S
build_mini mini_direct     mini_direct.S
build_mini mini_direct_xo  mini_direct_xo.S
build_mini mini_direct_ext mini_direct_ext.S
for v in $MINIS; do
  "$OBJDUMP" -s -j .gcc_except_table "$OUT/$v.o" > "$OUT/$v.objdump_s_except_table.txt"
  "$READELF" -x .eh_frame            "$OUT/$v.o" > "$OUT/$v.readelf_x_eh_frame.txt"
  "$READELF" -wf                     "$OUT/$v.o" > "$OUT/$v.readelf_wf.txt"
  "$READELF" -rW                     "$OUT/$v.o" > "$OUT/$v.readelf_r.txt"
  "$READELF" -sW                     "$OUT/$v.o" > "$OUT/$v.readelf_s.txt"
done
echo
echo "   逐字段解码 mini_indirect.o（手写 catch-all，0x9b 间接 personality）:"
"$PYTHON" decode_eh_lsda.py "$OUT/mini_indirect.o" | tee "$OUT/mini_indirect.decode.txt"
echo
echo "   personality 编码对比（CIEs 的 augmentation data 首字节）："
"$PYTHON" decode_eh_lsda.py "$OUT/mini_direct.o"    > "$OUT/mini_direct.decode.txt"
"$PYTHON" decode_eh_lsda.py "$OUT/mini_direct_xo.o" > "$OUT/mini_direct_xo.decode.txt"
"$PYTHON" decode_eh_lsda.py "$OUT/mini_direct_ext.o" > "$OUT/mini_direct_ext.decode.txt"
for v in $MINIS; do
  line=$("$PYTHON" - "$OUT/$v.o" <<'PY'
import json, subprocess, sys
d = json.loads(subprocess.check_output([sys.executable, "decode_eh_lsda.py", sys.argv[1], "--json"]))
enc = d["eh_frame"]["cies"][0]["encodings"]
print("personality_encoding=0x%02x  manifest=%s  aug_data=%s" % (
    enc["personality_encoding"], enc["personality_symbol"],
    d["eh_frame"]["cies"][0]["augmentation_data"]))
PY
)
  echo "     $v.o: $line"
done

echo
echo "## 6. 关键字节比对（手写 vs gcc；.o vs .exe）"
for sec in .gcc_except_table .eh_frame; do
  "$OBJCOPY" -O binary --only-section="$sec" "$OUT/probe_catch_all.O1.o" "$OUT/cmp_gcc_$sec.bin"
  "$OBJCOPY" -O binary --only-section="$sec" "$OUT/mini_indirect.o"     "$OUT/cmp_mini_$sec.bin"
  if cmp -s "$OUT/cmp_gcc_$sec.bin" "$OUT/cmp_mini_$sec.bin"; then
    echo "   [逐字节一致] $sec：mini_indirect.o == probe_catch_all.O1.o（$(wc -c < "$OUT/cmp_gcc_$sec.bin") 字节）"
    echo -n "                 "; od -An -tx1 -v "$OUT/cmp_gcc_$sec.bin" | tr -d ' \n'; echo
  else
    echo "   [不一致]     $sec"; cmp "$OUT/cmp_gcc_$sec.bin" "$OUT/cmp_mini_$sec.bin" || true
  fi
done
for f in $PROBES; do
  if cmp -s "$OUT/$f.obj.bin" "$OUT/$f.exe.bin"; then
    echo "   [一致]   $f：.o 与链接后 .exe 的 .gcc_except_table 相同"
  else
    echo "   [不同]   $f：链接后重定位被应用（TType 表项从 0 变成真实 pcrel 偏移）"
  fi
done
echo "   catch_int .exe LSDA 尾部 TType 项（已应用重定位）:"
echo -n "     "; od -An -tx1 -v "$OUT/probe_catch_int.exe.bin" | tr -d ' \n'; echo
echo "   catch_all .exe LSDA（无重定位，与 .o 相同）:"
echo -n "     "; od -An -tx1 -v "$OUT/probe_catch_all.exe.bin" | tr -d ' \n'; echo

echo
echo "## 7. 链接可行性矩阵（.cfi_personality 直接 0x1b vs gcc 同款间接 0x9b）"
link_case() {
  local label="$1"; shift
  local log="$OUT/link_$label.log"
  if "$@" >/dev/null 2>"$log"; then
    echo "   [OK]   $label"
  else
    echo "   [FAIL] $label"
    sed -n '1,3p' "$log" | sed 's/^/          /'
  fi
}
link_case mini_indirect_pie     "$CXX" -pie    -o "$OUT/mini_indirect.pie.exe"    "$OUT/mini_indirect.o" probe_main.cpp
link_case mini_indirect_nopie   "$CXX" -no-pie -o "$OUT/mini_indirect.nopie.exe"  "$OUT/mini_indirect.o" probe_main.cpp
link_case mini_indirect_shared  "$CXX" -shared -o "$OUT/mini_indirect.so"         "$OUT/mini_indirect.o"
link_case mini_direct_pie       "$CXX" -pie    -o "$OUT/mini_direct.pie.exe"      "$OUT/mini_direct.o" probe_main.cpp
link_case mini_direct_nopie     "$CXX" -no-pie -o "$OUT/mini_direct.nopie.exe"    "$OUT/mini_direct.o" probe_main.cpp
link_case mini_direct_shared    "$CXX" -shared -o "$OUT/mini_direct.so"           "$OUT/mini_direct.o"
link_case mini_direct_xo_pie    "$CXX" -pie    -o "$OUT/mini_direct_xo.pie.exe"   "$OUT/mini_direct_xo.o" probe_main.cpp
link_case mini_direct_xo_nopie  "$CXX" -no-pie -o "$OUT/mini_direct_xo.nopie.exe" "$OUT/mini_direct_xo.o" probe_main.cpp
link_case mini_direct_ext_pie   "$CXX" -pie    -o "$OUT/mini_direct_ext.pie.exe"  "$OUT/mini_direct_ext.o" probe_main.cpp
link_case mini_direct_ext_nopie "$CXX" -no-pie -o "$OUT/mini_direct_ext.nopie.exe" "$OUT/mini_direct_ext.o" probe_main.cpp
link_case mini_direct_ext_shared "$CXX" -shared -o "$OUT/mini_direct_ext.so"      "$OUT/mini_direct_ext.o"
echo
echo "   期望：0x9b(+DW.ref) 全通；0x1b 指向本可执行文件内的函数全通；"
echo "         0x1b 指向共享库里的 __gxx_personality_v0 时 PIE/shared 被 ld 拒绝、no-pie 可过。"

echo
echo "## 8. 汇总"
echo "   - 三个参考用例的 LSDA 十六进制："
for f in $PROBES; do
  echo -n "       $f: "
  od -An -tx1 -v "$OUT/$f.obj.bin" | tr -d ' \n'; echo
done
echo "   - 结论与逐条判定见 tools/lsda_ref/FINDINGS.md"
echo "   - 硬断言（可复现校验）: bash tools/lsda_ref/check.sh"
echo "   完成：所有产物已写入 $OUT/"
