#!/usr/bin/env bash
# 整程序产物缓存回归（0.478，P4 增量重编第一刀）
#
# 验的是「键覆盖全部链接输入」这条不变量：任何一项变化都必须 miss，
# 同输入必须 hit 且产物可运行。用法：
#   bash tests/progcache.sh [pyadamas 路径]
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PA=${1:-$ROOT/build/dev/pyadamas}
if [ ! -x "$PA" ]; then echo "progcache: 找不到可执行文件 $PA" >&2; exit 2; fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
export PYADAMAS_CACHE_DIR="$WORK/cache"
export PYADAMAS_PROGCACHE_DEBUG=1

fail=0
ok()  { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s: want %s, got %s\n' "$1" "$2" "$3"; fail=1; }

# 跑一次编译器（参数的其余部分），取 progcache 动作
pc() { "$PA" "$@" 2>&1 >/dev/null | grep -o 'progcache] \(store\|hit\)' | head -1 | sed 's/progcache] //'; }
expect() { local name=$1 want=$2; shift 2; local got; got=$(pc "$@"); [ "$got" = "$want" ] && ok "$name" || bad "$name" "$want" "$got"; }

printf 'print("hello")\n' > "$WORK/a.py"
printf 'print("hello2")\n' > "$WORK/b.py"

# 1) 首次 store；同输出路径 hit；换输出路径 hit 并写出产物
expect "首次构建 store" store build -O2 -o "$WORK/p1" "$WORK/a.py"
expect "同输出路径 hit" hit build -O2 -o "$WORK/p1" "$WORK/a.py"
got=$(pc build -O2 -o "$WORK/p2" "$WORK/a.py")
[ "$got" = "hit" ] && ok "换输出路径 hit" || bad "换输出路径 hit" hit "$got"
[ -x "$WORK/p2" ] && ok "缓存产物已写出且可执行" || bad "缓存产物可执行" yes no
got=$("$WORK/p2" 2>&1); [ "$got" = "hello" ] && ok "缓存产物输出正确" || bad "缓存产物输出" hello "$got"
cmp -s "$WORK/p1" "$WORK/p2" && ok "产物与输出路径无关（cmp 相同）" || bad "产物与输出路径无关" same different

# 2) 源码变更 / -O 档位 / --asan 都必须 miss
expect "源码变更 miss" store build -O2 -o "$WORK/p1" "$WORK/b.py"
expect "-O0 键不同" store build -O0 -o "$WORK/p3" "$WORK/b.py"
expect "-O2 回到旧键 hit" hit build -O2 -o "$WORK/p1" "$WORK/b.py"
expect "--asan 键不同" store build --asan -O2 -o "$WORK/p6" "$WORK/a.py"

# 3) 工具链指纹进键（同一份对象、不同编译器身份 → 必须 miss）
a=$(PYADAMAS_TOOLCHAIN_ID=A "$PA" build -O2 -o "$WORK/p4" "$WORK/a.py" 2>&1 >/dev/null | grep -o 'store\|hit' | head -1)
b=$(PYADAMAS_TOOLCHAIN_ID=B "$PA" build -O2 -o "$WORK/p5" "$WORK/a.py" 2>&1 >/dev/null | grep -o 'store\|hit' | head -1)
[ "$a" = "store" ] && [ "$b" = "store" ] && ok "toolchain 指纹进键" || bad "toolchain 进键" "store/store" "$a/$b"

# 4) 两个开关都绕过缓存
a=$(pc build --rebuild -O2 -o "$WORK/p1" "$WORK/b.py")
[ -z "$a" ] && ok "--rebuild 绕过缓存" || bad "--rebuild 绕过" "(无输出)" "$a"
a=$(PYADAMAS_NO_PROGCACHE=1 "$PA" build -O2 -o "$WORK/p1" "$WORK/b.py" 2>&1 >/dev/null | grep -c progcache)
[ "$a" = "0" ] && ok "PYADAMAS_NO_PROGCACHE=1 绕过缓存" || bad "NO_PROGCACHE" 0 "$a"

# 5) run 第二次命中且输出正确
"$PA" run "$WORK/a.py" >/dev/null 2>&1
out=$(PYADAMAS_PROGCACHE_DEBUG=1 "$PA" run "$WORK/a.py" 2>&1)
if echo "$out" | grep -q 'progcache] hit' && echo "$out" | grep -q 'hello'; then
    ok "run 命中且输出正确"
else
    bad "run 命中" "hit+hello" "$(echo "$out" | tr '\n' ' ')"
fi

# 6) 清空缓存后先 miss 再 hit
rm -rf "$PYADAMAS_CACHE_DIR/progcache"
expect "清缓存后 miss" store build -O2 -o "$WORK/p1" "$WORK/b.py"
expect "重建后 hit" hit build -O2 -o "$WORK/p1" "$WORK/b.py"

if [ "$fail" -eq 0 ]; then echo "progcache ok"; else echo "progcache FAILED"; fi
exit "$fail"
