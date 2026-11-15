#!/bin/bash
# =============================================================================
# tools/lsda_ref/check.sh —— M1 结论的可复现硬断言
#
# 做三件事：
#   1) 重新编译所有参考用例和手写 mini 用例（不复用 gen.sh 的旧产物）；
#   2) 用 decode_eh_lsda.py 重新解码，和 check_assert.py 里硬编码的字节逐项比对；
#   3) 重跑 0x1b/0x9b 的链接矩阵，断言哪些必须链接成功、哪些必须被 ld 拒绝。
# 任何一项不符：非零退出。
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")"

OUT=out
CXX=${CXX:-g++}
PYTHON=${PYTHON:-python3}
mkdir -p "$OUT"

FAILS=0
pass() { echo "  [PASS] $*"; }
fail() { echo "  [FAIL] $*"; FAILS=$((FAILS + 1)); }

echo "=============================================================="
echo " check.sh：重新编译 + 硬编码字节比对"
echo "=============================================================="

echo
echo "## 1. 重新编译 gcc 参考用例（-O1 -S 与 -O1 -c）"
for f in probe_catch_int probe_catch_all probe_cleanup; do
  "$CXX" -O1 -S -o "$OUT/$f.O1.s" "$f.cpp"
  "$CXX" -O1 -c -Wa,-L -o "$OUT/$f.O1.o" "$f.cpp"
  "$CXX" -O1 -o "$OUT/$f.exe" "$OUT/$f.O1.o" probe_main.cpp
  pass "编译 $f（.s / .o / .exe）"
done

echo
echo "## 2. 重新汇编手写 mini 用例"
"$CXX" -c -Wa,-L -o "$OUT/mini_indirect.o"   mini.S
"$CXX" -c -Wa,-L -o "$OUT/mini_direct.o"     mini_direct.S
"$CXX" -c -Wa,-L -o "$OUT/mini_direct_xo.o"  mini_direct_xo.S
"$CXX" -c -Wa,-L -o "$OUT/mini_direct_ext.o" mini_direct_ext.S
pass "汇编 mini.S / mini_direct.S / mini_direct_xo.S / mini_direct_ext.S"

echo
echo "## 3. 字节级断言（check_assert.py，期望值来自 FINDINGS.md）"
if "$PYTHON" check_assert.py; then
  pass "check_assert.py 全部通过"
else
  fail "check_assert.py 有断言失败"
fi

echo
echo "## 4. 链接矩阵：0x9b(+DW.ref) vs 0x1b(直接 pcrel)"
expect_ok() {
  local label="$1"; shift
  local log="$OUT/check_link_$label.log"
  if "$@" >/dev/null 2>"$log"; then
    pass "$label 链接成功"
  else
    fail "$label 应链接成功却失败"; sed -n '1,3p' "$log" | sed 's/^/         /'
  fi
}
expect_reject() {
  local label="$1" needle="$2"; shift 2
  local log="$OUT/check_link_$label.log"
  if "$@" >/dev/null 2>"$log"; then
    fail "$label 应被 ld 拒绝却链接成功"
  elif grep -qF "$needle" "$log"; then
    pass "$label 被 ld 拒绝（报错含：$needle）"
  else
    fail "$label 失败但报错里没有预期串：$needle"; sed -n '1,4p' "$log" | sed 's/^/         /'
  fi
}

# 0x9b + DW.ref 间接层：PIE / no-pie / shared 全通
expect_ok mini_indirect_pie    "$CXX" -pie    -o "$OUT/mini_indirect.pie.exe"   "$OUT/mini_indirect.o" probe_main.cpp
expect_ok mini_indirect_nopie  "$CXX" -no-pie -o "$OUT/mini_indirect.nopie.exe" "$OUT/mini_indirect.o" probe_main.cpp
expect_ok mini_indirect_shared "$CXX" -shared -o "$OUT/mini_indirect.so"        "$OUT/mini_indirect.o"

# 0x1b 直接指向本地符号：可以省掉 DW.ref
expect_ok mini_direct_pie      "$CXX" -pie    -o "$OUT/mini_direct.pie.exe"     "$OUT/mini_direct.o" probe_main.cpp
expect_ok mini_direct_nopie    "$CXX" -no-pie -o "$OUT/mini_direct.nopie.exe"   "$OUT/mini_direct.o" probe_main.cpp
expect_ok mini_direct_shared   "$CXX" -shared -o "$OUT/mini_direct.so"          "$OUT/mini_direct.o"

# 0x1b 直接指向“同一可执行文件里另一个 TU”的符号：也可以
expect_ok mini_direct_xo_pie   "$CXX" -pie    -o "$OUT/mini_direct_xo.pie.exe"  "$OUT/mini_direct_xo.o" probe_main.cpp
expect_ok mini_direct_xo_nopie "$CXX" -no-pie -o "$OUT/mini_direct_xo.nopie.exe" "$OUT/mini_direct_xo.o" probe_main.cpp

# 0x1b 直接指向共享库符号：PIE/shared 必须被拒绝，no-pie 可通过
expect_reject mini_direct_ext_pie \
  "relocation R_X86_64_PC32 against symbol" \
  "$CXX" -pie -o "$OUT/mini_direct_ext.pie.exe" "$OUT/mini_direct_ext.o" probe_main.cpp
expect_reject mini_direct_ext_shared \
  "can not be used when making a shared object" \
  "$CXX" -shared -o "$OUT/mini_direct_ext.so" "$OUT/mini_direct_ext.o"
expect_ok mini_direct_ext_nopie \
  "$CXX" -no-pie -o "$OUT/mini_direct_ext.nopie.exe" "$OUT/mini_direct_ext.o" probe_main.cpp

echo
echo "=============================================================="
if [ "$FAILS" -ne 0 ]; then
  echo "结果：$FAILS 项失败"
  exit 1
fi
echo "结果：全部断言通过（exit 0）"
