#!/usr/bin/env bash
# 分层依赖检查（0.606）：模块之间只允许「向上」依赖。
#
#   common  <-  syntax  <-  typecheck  <-  lower  <-  opt  <-  backend  <-  driver
#
# 违反的例子：typecheck 里 include backend、lower 里 include driver。
# 这类依赖会让改动沿着反向传播，是「模块臃肿」的根源之一。
set -u
cd "$(dirname "$0")/.." || exit 1

fail=0
check() {
    local dir="$1" forbid="$2" why="$3"
    local hits
    hits=$(grep -rn "#include \"$forbid" "src/$dir" 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "[分层违规] src/$dir 不应依赖 $forbid（$why）"
        echo "$hits" | head -5
        fail=1
    fi
}

check syntax    "typecheck/" "语法层不该知道类型检查"
check syntax    "lower/"     "语法层不该知道降级"
check typecheck "lower/"     "类型检查不该知道降级"
check typecheck "backend/"   "类型检查不该知道后端"
check lower     "backend/"   "降级不该依赖具体后端"
check lower     "driver/"    "降级不该知道驱动"
check opt       "driver/"    "优化不该知道驱动"

if [ "$fail" -ne 0 ]; then
    echo "结果：分层依赖检查失败。"
    exit 1
fi
echo "结果：分层依赖检查通过。"