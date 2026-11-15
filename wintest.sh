#!/bin/bash
# wintest.sh —— Windows(win64) 目标的构建/运行辅助
#   winrt              重建 Windows 版运行时（改了 runtime/*.c 之后要跑）
#   winbuild <f.py>    编译成 win64 exe，回显路径
#   winrun  <f.py>     编译并运行（10s 超时），打印 EXIT/stdout/stderr
# 说明：WSL 可直接执行 .exe 并拿到真实退出码，不需要 PowerShell。
set -u
WINRUN=/mnt/c/Users/lenovo/AppData/Local/Temp/winrun
RTOBJ=/tmp/winrt/rt_win.o

winrt() {
  mkdir -p /tmp/winrt
  local objs=""
  for f in runtime/rt_*.c; do
    local b=$(basename "$f" .c)
    # rt_subprocess 已移植      # 尚未移植
    x86_64-w64-mingw32-gcc -O1 -c "$f" -I runtime -o /tmp/winrt/$b.o 2>/dev/null && objs="$objs /tmp/winrt/$b.o"
  done
  x86_64-w64-mingw32-ld -r -o "$RTOBJ" $objs && echo "rt_win.o ok"
}

winbuild() {
  local src=$1
  local b=$(basename "$src" .py)
  mkdir -p "$WINRUN"
  ./build/dev/pyadamas --rebuild asm --target=win64 "$src" > "$WINRUN/$b.s" 2>"$WINRUN/$b.asm.log" || { echo "ASM_FAIL: $(head -1 "$WINRUN/$b.asm.log")"; return 1; }
  x86_64-w64-mingw32-gcc -c "$WINRUN/$b.s" -o "$WINRUN/$b.o" 2>"$WINRUN/$b.cc.log" || { echo "CC_FAIL: $(head -1 "$WINRUN/$b.cc.log")"; return 1; }
  x86_64-w64-mingw32-gcc "$WINRUN/$b.o" "$RTOBJ" -o "$WINRUN/$b.exe" -lws2_32 2>"$WINRUN/$b.ld.log" || { echo "LD_FAIL: $(head -1 "$WINRUN/$b.ld.log")"; return 1; }
  echo "$WINRUN/$b.exe"
}

winrun() {
  local src=$1
  local b=$(basename "$src" .py)
  local exe
  exe=$(winbuild "$src") || { echo "$exe"; return 1; }
  timeout 10 "$exe" > "$WINRUN/$b.out" 2> "$WINRUN/$b.err"
  local rc=$?
  echo "EXIT=$rc"
  echo "--- stdout ---"; sed 's/\r$//' "$WINRUN/$b.out"
  echo "--- stderr ---"; sed 's/\r$//' "$WINRUN/$b.err"
}

"$@"
