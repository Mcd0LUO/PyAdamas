#!/usr/bin/env bash
# 前端鲁棒性基线的统一采集脚本（0.507）。
#
# 为什么需要它：这个数字被三个代理各自量过，口径来回漂（628/626 个文件、
# 默认口径 495/496/499 三个版本），每次都要在文档里解释「为什么和上次不一样」。
# 统一成一条命令，输出四个口径的数字，谁量都一样。
#
# 用法：bash tools/frontend_baseline.sh [stdlib 目录] [pyadamas 路径]
#   默认：/usr/lib/python3.14  与 build/lead/pyadamas
#
# 口径：
#   syntax   check --syntax        只做词法/语法
#   default  check                 当前默认（strict names）
#   old      check --no-imports --lenient-names   0.171 时代的旧范式
#   imports  check --imports       加载 import 图做模块属性检查（更严，会因签名表不全误杀）
set -u
STDLIB=${1:-/usr/lib/python3.14}
PA=${2:-build/lead/pyadamas}
[ -x "$PA" ] || { echo "找不到编译器：$PA" >&2; exit 2; }
[ -d "$STDLIB" ] || { echo "找不到标准库目录：$STDLIB" >&2; exit 2; }

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1

FILES=$(find "$STDLIB" -name '*.py' -type f | sort)
TOTAL=$(printf '%s\n' "$FILES" | wc -l)

count() {
    local mode="$*"
    printf '%s\n' "$FILES" | xargs -P8 -I{} sh -c "\"\$0\" check $mode \"\$1\" >/dev/null 2>&1 && echo ok" "$PA" {} | wc -l
}

echo "stdlib: $STDLIB"
echo "binary: $PA（$( "$PA" deps --json /dev/stdin <<< 'x=1' 2>/dev/null | grep -o '"version": "[^"]*"' | head -1)）"
echo "files:  $TOTAL"
echo "syntax:  $(count --syntax)/$TOTAL"
echo "default: $(count)/$TOTAL"
echo "old:     $(count --no-imports --lenient-names)/$TOTAL"
echo "imports: $(count --imports)/$TOTAL"