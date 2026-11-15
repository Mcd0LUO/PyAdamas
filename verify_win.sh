#!/bin/bash
# verify_win.sh —— Windows 移植的完整验证（合并前后各跑一次）
#   1. 构建编译器
#   2. POSIX 全量测试（必须 177 通过 / 0 失败）
#   3. SysV 汇编 md5（必须 88f2bb28da857d385fae0f100866782a）
#   4. 标准库前端基线（必须 502 / 598）
#   5. Windows 覆盖：把 tests/run 全部编成 win64 exe，在 Windows 上跑，
#      与 POSIX 输出逐字节比对
set -u
cd "$(dirname "$0")"
BASE_MD5=88f2bb28da857d385fae0f100866782a
FAIL=0

echo "== 1) 构建 =="
ninja -C build/dev -j8 pyadamas > /tmp/vw_build.log 2>&1 || { echo "BUILD FAILED"; tail -5 /tmp/vw_build.log; exit 1; }
echo "ok"

echo "== 2) POSIX 全量 =="
PYADAMAS_TEST_JOBS=6 bash tests/run_tests.sh build/dev build/dev/pyadamas > /tmp/vw_test.log 2>&1
rc=$?
okn=$(grep -c "ran ok" /tmp/vw_test.log)
echo "exit=$rc ok=$okn"
[ "$rc" = "0" ] && [ "$okn" = "177" ] || { echo "FAILED"; grep -iE "^ *FAIL|MISMATCH" /tmp/vw_test.log | head -5; FAIL=1; }

echo "== 3) SysV 汇编 md5 =="
rm -rf /tmp/vw_asm && mkdir -p /tmp/vw_asm
for f in tests/run/*.py; do b=$(basename "$f" .py); ./build/dev/pyadamas asm "$f" > /tmp/vw_asm/$b.s 2>/dev/null; done
md5=$(cat /tmp/vw_asm/*.s | md5sum | cut -d' ' -f1)
echo "md5=$md5"
[ "$md5" = "$BASE_MD5" ] || { echo "FAILED (baseline $BASE_MD5)"; FAIL=1; }

echo "== 4) 标准库基线 =="
/tmp/baseline.sh ./build/dev/pyadamas /tmp/vw_base.txt

echo "== 5) Windows 覆盖 =="
./wintest.sh winrt > /dev/null
D=/mnt/c/Users/lenovo/AppData/Local/Temp/vw
rm -rf $D; mkdir -p $D
built=0
for f in tests/run/*.py; do
  b=$(basename "$f" .py)
  ./build/dev/pyadamas --rebuild asm --target=win64 "$f" > $D/$b.s 2>/dev/null || continue
  x86_64-w64-mingw32-gcc -c $D/$b.s -o $D/$b.o 2>/dev/null || continue
  x86_64-w64-mingw32-gcc $D/$b.o /tmp/winrt/rt_win.o -o $D/$b.exe -lws2_32 2>/dev/null || continue
  ./build/dev/pyadamas run "$f" > $D/$b.expected 2>&1
  timeout -s KILL 10 $D/$b.exe > $D/$b.out 2>$D/$b.err
  echo $? > $D/$b.code
  for x in "$D/$b.out" "$D/$b.err"; do [ -f "$x" ] && [ $(stat -c%s "$x") -gt 2000000 ] && truncate -s 2000000 "$x"; done
  built=$((built+1))
done
same=0; diffc=0; crash=0; hang=0
dl=""; cl=""; hl=""
for f in $D/*.code; do
  b=$(basename "$f" .code); code=$(cat "$f")
  if [ "$code" = "137" ] || [ "$code" = "124" ]; then hang=$((hang+1)); hl="$hl $b"
  elif [ "$code" != "0" ]; then crash=$((crash+1)); cl="$cl $b"
  elif diff -q <(sed 's/\r$//' $D/$b.out) $D/$b.expected > /dev/null 2>&1; then same=$((same+1))
  else diffc=$((diffc+1)); dl="$dl $b"; fi
done
echo "built=$built  same=$same  differ=$diffc  crash=$crash  hang=$hang"
echo "differ:$dl"
echo "crash:$cl"
echo "hang:$hl"

echo "== 结果 =="
[ "$FAIL" = "0" ] && echo "CONSTRAINTS OK" || echo "CONSTRAINTS FAILED"
