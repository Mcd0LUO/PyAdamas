#!/usr/bin/env bash
# check_size.sh —— 工程规模检查
#
# 规则：
#   1. 单个源文件超过 1200 行 → 直接失败（允许清单里的除外）。
#   2. 单个函数超过 300 行 → 只打印警告，不失败。
#      （函数长度是重构提示，不是硬约束；文件规模才是硬约束。）
#
# 用法：bash tests/check_size.sh
# 退出码：0 全部通过；1 有文件超限。
set -u

cd "$(dirname "$0")/.." || exit 2

FILE_LIMIT=1200
FUNC_LIMIT=300

# 仍待拆分的大文件。当前为空：所有源文件都已拆到 1200 行以内。
# 这个清单只能删不能加；新增文件超过上限时必须拆分，不要登记进来。
ALLOW_FILES=""

is_allowed() {
    case "
$ALLOW_FILES
" in
        *"
$1
"*) return 0 ;;
        *) return 1 ;;
    esac
}

fail=0

echo "== 文件行数（硬限制 ${FILE_LIMIT}）=="
for f in $(find src -type f \( -name '*.cpp' -o -name '*.h' \)); do
    [ -e "$f" ] || continue
    n=$(wc -l < "$f")
    if [ "$n" -gt "$FILE_LIMIT" ]; then
        if is_allowed "$f"; then
            echo "  允许  $f  $n 行"
        else
            echo "  超限  $f  $n 行（> ${FILE_LIMIT}）"
            fail=1
        fi
    fi
done

echo
echo "== 函数行数（仅警告，> ${FUNC_LIMIT}）=="
# 只认从第 1 列开始、以 '{' 结尾的定义（顶层函数/类/结构体），
# 用花括号配平算长度；字符串里的花括号先剥掉。
warned=0
for f in $(find src -type f \( -name '*.cpp' -o -name '*.h' \)); do
    [ -e "$f" ] || continue
    out=$(awk -v limit="$FUNC_LIMIT" '
        BEGIN { depth = 0; start = 0; name = "" }
        {
            line = $0
            gsub(/"[^"]*"/, "", line)
            # namespace 不是函数：整行跳过，避免把整个文件算成一个函数。
            if (line ~ /^namespace[ \t]/ || line ~ /^\}[ \t]*\/\/[ \t]*namespace/)
                next
            if (depth == 0 && line ~ /^[A-Za-z_].*\{[ \t]*$/) {
                start = NR
                name = $0
                sub(/[ \t]*\{[ \t]*$/, "", name)
            }
            opens = gsub(/\{/, "{", line)
            closes = gsub(/\}/, "}", line)
            depth += opens - closes
            if (start > 0 && depth == 0) {
                len = NR - start + 1
                if (len > limit)
                    printf "  警告  %s:%d  %d 行  %s\n", FILENAME, start, len, name
                start = 0
            }
        }
    ' "$f")
    if [ -n "$out" ]; then
        echo "$out"
        warned=1
    fi
done
[ "$warned" -eq 0 ] && echo "  （无）"

echo
if [ "$fail" -ne 0 ]; then
    echo "结果：文件规模超限。请拆分，或把确实需要保留的文件登记到 ALLOW_FILES。"
    exit 1
fi

# 生成物一致性：rtfn_names.h 由 src/ir/rtfn.def 生成，漂移就地拦住。
if ! python3 "$(dirname "$0")/../tools/gen_rtfn.py" --check-enum >/dev/null 2>&1; then
    echo "结果：src/ir/rtfn.h 的枚举与 src/ir/rtfn.def 不一致（顺序必须完全一致）。"
    exit 1
fi
if ! python3 "$(dirname "$0")/../tools/gen_rtfn.py" --check-groups >/dev/null 2>&1; then
    echo "结果：src/ir/rtfn.def 的参数编组与 backend/call_rt.cpp 不一致。"
    exit 1
fi
if ! python3 "$(dirname "$0")/../tools/gen_rtfn.py" --check >/dev/null 2>&1; then
    echo "结果：src/ir/rtfn_names.h 与 src/ir/rtfn.def 不一致（运行 tools/gen_rtfn.py 重新生成）。"
    exit 1
fi

# 分层依赖：模块只允许向上依赖（common <- syntax <- typecheck <- lower <- opt <- backend <- driver）。
if ! bash "$(dirname "$0")/../tools/check_layers.sh" >/dev/null 2>&1; then
    echo "结果：分层依赖检查失败（运行 tools/check_layers.sh 看详情）。"
    exit 1
fi

echo "结果：文件规模检查通过。"
