#!/usr/bin/env bash
# check --imports 的口径回归（0.486）：模块属性检查是 opt-in，且必须三件事同时成立——
#   1) 默认 check 不加载 import 图：json.nonexistent_attr 通过（不误杀真实代码，
#      我们的 stdlib 签名表不全，默认打开会让 subprocess.check_output /
#      sys.version_info 这类未建模成员一律判错，实测通过率 5/20 → 12/20）；
#   2) check --imports 对同一文件失败，消息是 module 'json' has no attribute ...；
#   3) check --imports 下子模块属性链 os.path.join 仍能解析（0.484 的能力不回退）。
#
# 用法: check_imports.sh <build_dir> <pyadamas>
set -u
BUILD_DIR=${1:?usage: check_imports.sh <build_dir> <pyadamas>}
PA=${2:?usage: check_imports.sh <build_dir> <pyadamas>}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export PYADAMAS_CACHE_DIR="$work/cache"
mkdir -p "$work/cache"

cat > "$work/modattr.py" <<'PYEOF'
import json

def main() -> None:
    x: int = json.nonexistent_attr(1)
    print(x)

main()
PYEOF

cat > "$work/ospath.py" <<'PYEOF'
import os

print(os.path.join("a", "b"))
printf_ok: int = 0
PYEOF

fail=0

# 1) 默认：必须通过（模块属性不校验）
if "$PA" check "$work/modattr.py" >"$work/def.log" 2>&1; then
    echo "  default check modattr: passed (ok, no import graph)"
else
    echo "  default check modattr: FAILED (should pass; --imports must stay opt-in)"
    sed -n '1,6p' "$work/def.log"
    fail=1
fi

# 2) --imports：必须失败，且是模块属性消息
if "$PA" check --imports "$work/modattr.py" >"$work/imp.log" 2>&1; then
    echo "  check --imports modattr: PASSED (should fail)"
    fail=1
elif grep -qF "module 'json' has no attribute 'nonexistent_attr'" "$work/imp.log"; then
    echo "  check --imports modattr: rejected (module 'json' has no attribute ...)"
else
    echo "  check --imports modattr: WRONG diagnostic"
    sed -n '1,6p' "$work/imp.log"
    fail=1
fi

# 3) --imports 下子模块属性链仍然解析
if "$PA" check --imports "$work/ospath.py" >"$work/ospath.log" 2>&1; then
    echo "  check --imports os.path chain: passed"
else
    echo "  check --imports os.path chain: FAILED (0.484 regression)"
    sed -n '1,6p' "$work/ospath.log"
    fail=1
fi

# 4) 循环 import 必须能终止（3.13：接口编码 UAF / 无限递归会段错误 rc=139）。
#    循环依赖本身合法：要么通过，要么报类型错，绝不能崩。
cat > "$work/cyc_a.py" <<'PYEOF'
import cyc_b

def fact(n: int):
    if n <= 1:
        return 1
    return n * fact(n - 1)

def use() -> int:
    return cyc_b.helper(fact(3))

print(use())
PYEOF

cat > "$work/cyc_b.py" <<'PYEOF'
import cyc_a

def helper(n: int) -> int:
    return n + 1

def depth(n: int):
    if n <= 0:
        return 0
    return 1 + depth(n - 1)
PYEOF

"$PA" check --imports -I "$work" "$work/cyc_a.py" >"$work/cyc.log" 2>&1
cyc_rc=$?
if [ $cyc_rc -le 1 ]; then
    echo "  check --imports cyclic import: terminated (rc=$cyc_rc, ok)"
else
    echo "  check --imports cyclic import: CRASHED (rc=$cyc_rc)"
    sed -n '1,6p' "$work/cyc.log"
    fail=1
fi

# 5) A3-1：依赖的语法错误不能被吞。以前 catch(...) 把 LexError/ParseError 和
#    bad_alloc 一视同仁 → 记 missing 按 unknown 继续，check 返回 ok（还给误导的
#    「parsed but not yet compiled」提示）；build 端是直接拒绝的。
mkdir -p "$work/badep"
cat > "$work/badep/badep.py" <<'PYEOF'
def f(:
    pass
PYEOF
cat > "$work/badep/usesbad.py" <<'PYEOF'
import badep

print(badep.x)
PYEOF
"$PA" check -I "$work/badep" "$work/badep/usesbad.py" >"$work/badep.log" 2>&1
if [ $? -eq 0 ]; then
    echo "  check dependency syntax error: PASSED (should fail, A3-1)"
    sed -n '1,6p' "$work/badep.log"
    fail=1
elif grep -qF "expected parameter name" "$work/badep.log"; then
    echo "  check dependency syntax error: rejected (expected parameter name ...)"
else
    echo "  check dependency syntax error: WRONG diagnostic (A3-1)"
    sed -n '1,6p' "$work/badep.log"
    fail=1
fi

# 6) A3-2：同一个模块名解析到两个不同文件。check 以前按 name 记忆化，会把
#    第二个文件当成第一个模块的接口（实测 check ok / build 报错）。
mkdir -p "$work/two/pkg"
cat > "$work/two/helper.py" <<'PYEOF'
X = 1
PYEOF
cat > "$work/two/pkg/helper.py" <<'PYEOF'
X = 2
PYEOF
cat > "$work/two/pkg/__init__.py" <<'PYEOF'
import helper
Y = helper.X
PYEOF
cat > "$work/two/main.py" <<'PYEOF'
import pkg
import helper

print(helper.X, pkg.Y)
PYEOF
"$PA" check -I "$work/two" "$work/two/main.py" >"$work/two.log" 2>&1
if [ $? -eq 0 ]; then
    echo "  check same name two files: PASSED (should fail, A3-2)"
    sed -n '1,6p' "$work/two.log"
    fail=1
elif grep -qF "resolves to two different files" "$work/two.log"; then
    echo "  check same name two files: rejected (resolves to two different files)"
else
    echo "  check same name two files: WRONG diagnostic (A3-2)"
    sed -n '1,6p' "$work/two.log"
    fail=1
fi

if [ $fail -ne 0 ]; then
    echo "check --imports 口径回归失败"
    exit 1
fi
echo "check --imports 口径 6/6 通过"