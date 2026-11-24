#!/usr/bin/env bash
# 包管理 P0 回归（docs/package-management-design.md §6.1 六条验收 + 扩展项）
# 用法：tests/pkg/run.sh <build-dir> <pyadamas 可执行文件>
#
# 每个检查独立命名、失败时打印命令回显，便于直接定位。工作区在 mktemp 目录，
# 模块缓存也隔离在那里面（PYADAMAS_CACHE_DIR），不污染 build/ 的共享缓存。
set -u

if [ $# -lt 2 ]; then
    echo "usage: tests/pkg/run.sh <build-dir> <pyadamas binary>" >&2
    exit 2
fi
BUILD=$1
PY=$2
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1
case "$PY" in
    /*) ;;
    *) PY="$ROOT/$PY" ;;
esac
if [ ! -x "$PY" ]; then
    echo "pkg: pyadamas binary not executable: $PY" >&2
    exit 2
fi

TMPBASE=$TMPDIR
if [ -z "$TMPBASE" ]; then
    TMPBASE=/tmp
fi
WORK=$(mktemp -d "$TMPBASE/pyadamas-pkg-XXXXXX") || exit 1
trap 'rm -rf "$WORK"' EXIT
export PYADAMAS_CACHE_DIR="$WORK/modcache"
OUT="$WORK/.out"
ERR="$WORK/.err"

checks=0
fail=0
ok() {
    checks=$((checks + 1))
    printf '  %-56s ok\n' "$1"
}
bad() {
    checks=$((checks + 1))
    fail=1
    printf '  %-56s FAIL\n' "$1"
    shift
    for line in "$@"; do
        printf '      %s\n' "$line"
    done
}

# 运行并记录 stdout/stderr/退出码到 $OUT / $ERR / $rc。
run() {
    "$@" >"$OUT" 2>"$ERR"
    rc=$?
}

expect_exit_ok() { # name
    if [ "$rc" -eq 0 ]; then
        ok "$1"
    else
        bad "$1" "exit=$rc (want 0)"
        sed -n '1,8p' "$OUT"
        sed -n '1,8p' "$ERR"
    fi
}

expect_ok() { # name needle
    if [ "$rc" -eq 0 ] && grep -qF -- "$2" "$OUT"; then
        ok "$1"
    else
        bad "$1" "exit=$rc (want 0)" "stdout must contain: $2"
        sed -n '1,8p' "$OUT"
        sed -n '1,8p' "$ERR"
    fi
}

expect_fail() { # name needle(stderr)
    if [ "$rc" -ne 0 ] && grep -qF -- "$2" "$ERR"; then
        ok "$1"
    else
        bad "$1" "exit=$rc (want nonzero)" "stderr must contain: $2"
        sed -n '1,10p' "$ERR"
    fi
}

expect_grep() { # name file needle
    if grep -qF -- "$3" "$2" 2>/dev/null; then
        ok "$1"
    else
        bad "$1" "missing: $3" "in: $2"
    fi
}

expect_not_grep() { # name file ERE
    if grep -qE -- "$3" "$2" 2>/dev/null; then
        bad "$1" "unexpected match: $3" "in: $2"
        grep -nE -- "$3" "$2" | head -5
    else
        ok "$1"
    fi
}

# ── 夹具：proj（有清单）+ 依赖在项目内部（整体搬家后相对路径不变）──
PROJ="$WORK/proj"
mkdir -p "$PROJ/deps/libfoo/foo" "$PROJ/deps/libbar"

cat >"$PROJ/pyadamas.toml" <<'EOF'
[package]
name = "app"
version = "0.1.0"

[dependencies]
libfoo = { path = "deps/libfoo" }
libbar = { path = "deps/libbar" }

[modules]
foo = { package = "libfoo", module = "foo" }
"foo.util" = { package = "libfoo", module = "foo.util" }
bar = { package = "libbar", module = "bar" }
EOF
cp "$PROJ/pyadamas.toml" "$WORK/good.toml"

cat >"$PROJ/deps/libfoo/pyadamas.toml" <<'EOF'
[package]
name = "libfoo"
version = "0.2.0"
EOF
cat >"$PROJ/deps/libfoo/foo/__init__.py" <<'EOF'
def hello():
    return "foo"
EOF
cat >"$PROJ/deps/libfoo/foo/util.py" <<'EOF'
def twice(x: int) -> int:
    return x * 2
EOF
cat >"$PROJ/deps/libbar/pyadamas.toml" <<'EOF'
[package]
name = "libbar"
version = "0.3.0"

[dependencies]
libfoo = { path = "../libfoo" }
EOF
cat >"$PROJ/deps/libbar/bar.py" <<'EOF'
def hello():
    return "bar"
EOF
cat >"$PROJ/main.py" <<'EOF'
import foo
import foo.util as util
import bar

print(foo.hello(), bar.hello(), util.twice(21))
EOF

echo "== pkg: manifest / lock / resolve / --locked =="

# ① 无清单项目 0 回归：老路径照旧；不产生锁；--locked 明确报错而不是静默放过。
PLAIN="$WORK/plain"
mkdir -p "$PLAIN/helper"
cat >"$PLAIN/helper/__init__.py" <<'EOF'
def value() -> int:
    return 7
EOF
cat >"$PLAIN/main.py" <<'EOF'
import helper
print(helper.value())
EOF
run "$PY" build "$PLAIN/main.py" -o "$WORK/plain_app"
expect_exit_ok "no-manifest: build"
run "$WORK/plain_app"
expect_ok "no-manifest: run prints 7" "7"
if [ -e "$PLAIN/pyadamas.lock" ]; then
    bad "no-manifest: no lock is created" "unexpected '$PLAIN/pyadamas.lock'"
else
    ok "no-manifest: no lock is created"
fi
run "$PY" build --locked "$PLAIN/main.py" -o "$WORK/plain_app2"
expect_fail "no-manifest: --locked fails clearly" "needs a pyadamas.toml"

# ② path 依赖跨目录编译通过（模块只能从锁派生的映射进来）。
run "$PY" resolve "$PROJ"
expect_ok "resolve writes the lock" "wrote "
run "$PY" build "$PROJ/main.py" -o "$WORK/app"
expect_exit_ok "path dep: build"
run "$WORK/app"
expect_ok "path dep: run prints foo bar 42" "foo bar 42"
# map 不在源码旁边：从另一个 cwd 用绝对入口 + --locked 构建（派生 map 在 /tmp）。
run "$PY" build --locked "$PROJ/main.py" -o "$WORK/app_from_elsewhere"
expect_exit_ok "path dep: build from another cwd (--locked)"
if [ -x "$WORK/app_from_elsewhere" ]; then
    run "$WORK/app_from_elsewhere"
    expect_ok "path dep: binary from another cwd runs" "foo bar 42"
fi

# 有清单时本地模块仍走路径搜索（§5.9.1：新机制不侵入老机制），且改项目源码
# 不该让锁失效（锁只描述依赖）。
cat >"$PROJ/localmod.py" <<'EOF'
def value() -> int:
    return 9
EOF
cat >"$PROJ/local_entry.py" <<'EOF'
import localmod
print(localmod.value())
EOF
run "$PY" build --locked "$PROJ/local_entry.py" -o "$WORK/local_app"
expect_exit_ok "local modules still resolve by path with a manifest"
run "$WORK/local_app"
expect_ok "local module runs" "9"

LOCK="$PROJ/pyadamas.lock"
expect_grep "lock: root coordinate" "$LOCK" "root app 0.1.0"
expect_grep "lock: libbar package line" "$LOCK" 'pkg libbar@0.3.0 path="deps/libbar"'
expect_grep "lock: libfoo package line" "$LOCK" 'pkg libfoo@0.2.0 path="deps/libfoo"'
expect_grep "lock: module mapping" "$LOCK" 'map foo.util libfoo foo.util "foo/util.py"'
expect_grep "lock: transitive dep edge keeps authored path" "$LOCK" \
    'dep libbar libfoo path="../libfoo"'
if grep -qE '^pkg .* tree=sha256:[0-9a-f]{64}$' "$LOCK" &&
    [ "$(grep -cE '^pkg .* tree=sha256:[0-9a-f]{64}$' "$LOCK")" = 2 ]; then
    ok "lock: every package has a sha256 tree hash"
else
    bad "lock: every package has a sha256 tree hash" "$(grep '^pkg ' "$LOCK")"
fi

# ③ 手改清单后 --locked 失败并打印 diff（含下一步）。
cat >"$PROJ/pyadamas.toml" <<'EOF'
[package]
name = "app"
version = "0.1.0"

[dependencies]
libfoo = { path = "deps/libfoo" }
libbar = { path = "deps/libbar" }

[modules]
foo = { package = "libfoo", module = "foo" }
"foo.util" = { package = "libbar", module = "bar" }
bar = { package = "libbar", module = "bar" }
EOF
run "$PY" build --locked "$PROJ/main.py" -o "$WORK/app_stale"
expect_fail "locked: edited manifest fails" "pyadamas.lock is out of date"
expect_grep "locked: diff names the changed mapping" "$ERR" "~ map foo.util:"
expect_grep "locked: diff says how to fix" "$ERR" "run 'pyadamas resolve'"
# 非 --locked 是宽松模式：按清单重解析并打 note，但不阻断构建。
# 这里只改 root 版本（不改变模块图），证明「过期但可构建」不会被误拦。
sed 's/version = "0.1.0"/version = "0.2.0"/' "$WORK/good.toml" >"$PROJ/pyadamas.toml"
run "$PY" build "$PROJ/main.py" -o "$WORK/app_loose"
expect_exit_ok "loose mode: stale lock does not block the build"
expect_grep "loose mode: stale lock prints a note" "$ERR" "note: pyadamas.lock is out of date"
expect_grep "loose mode: stale lock prints the diff" "$ERR" "~ root app@0.1.0 -> app@0.2.0"
cp "$WORK/good.toml" "$PROJ/pyadamas.toml"
run "$PY" resolve "$PROJ"
run "$PY" build --locked "$PROJ/main.py" -o "$WORK/app_restored"
expect_exit_ok "locked: resolving restores a passing lock"

# 锁缺失也必须失败，并列出「将会锁什么」。
mv "$LOCK" "$WORK/saved.lock"
run "$PY" build --locked "$PROJ/main.py" -o "$WORK/app_nolock"
expect_fail "locked: missing lock fails" "pyadamas.lock is missing"
expect_grep "locked: missing lock shows what would be pinned" "$ERR" "+ package libfoo@0.2.0"
mv "$WORK/saved.lock" "$LOCK"

# ④ 连续两次 resolve 字节相同（含「不给路径、用 cwd」的形式）。
run "$PY" resolve "$PROJ"
cp "$LOCK" "$WORK/lock.first"
run "$PY" resolve "$PROJ"
cp "$LOCK" "$WORK/lock.second"
if cmp -s "$WORK/lock.first" "$WORK/lock.second"; then
    ok "determinism: two resolves are byte-identical"
else
    bad "determinism: two resolves are byte-identical" "cmp failed"
    diff -u "$WORK/lock.first" "$WORK/lock.second" | head -20
fi
( cd "$PROJ" && "$PY" resolve >"$OUT" 2>"$ERR" )
rc=$?
cp "$LOCK" "$WORK/lock.cwd"
if [ "$rc" -eq 0 ] && cmp -s "$WORK/lock.first" "$WORK/lock.cwd"; then
    ok "determinism: resolve in cwd matches resolve <dir>"
else
    bad "determinism: resolve in cwd matches resolve <dir>" "exit=$rc" "$(sed -n '1,6p' "$ERR")"
fi
run "$PY" resolve --manifest "$PROJ/pyadamas.toml"
cp "$LOCK" "$WORK/lock.flag"
if [ "$rc" -eq 0 ] && cmp -s "$WORK/lock.first" "$WORK/lock.flag"; then
    ok "determinism: resolve --manifest matches resolve <dir>"
else
    bad "determinism: resolve --manifest matches resolve <dir>" "exit=$rc" "$(sed -n '1,6p' "$ERR")"
fi

# ⑤ 项目整体移动目录后 lock 不变。
cp -a "$PROJ" "$WORK/proj-moved"
run "$PY" resolve "$WORK/proj-moved"
if cmp -s "$LOCK" "$WORK/proj-moved/pyadamas.lock"; then
    ok "relocatable: lock is identical after moving the project"
else
    bad "relocatable: lock is identical after moving the project" "cmp failed"
    diff -u "$LOCK" "$WORK/proj-moved/pyadamas.lock" | head -20
fi

# ⑥ 锁里没有绝对路径（换机器/换目录必须仍然有效）。
if grep -qF "$WORK" "$LOCK"; then
    bad "portability: lock contains no workdir path" "found '$WORK'"
else
    ok "portability: lock contains no workdir path"
fi
expect_not_grep "portability: no path starting with /" "$LOCK" 'path="/'
expect_not_grep "portability: no quoted absolute string" "$LOCK" '"/[^"]*"'

# ── 扩展：哈希、失效、缺失、格式错误 ──────────────────────────
OLD_HASH=$(sed -nE 's/^pkg libfoo@0\.2\.0 .*tree=sha256:([0-9a-f]{64})$/\1/p' "$LOCK")
printf '\n# tampered for the pkg test\n' >>"$PROJ/deps/libfoo/foo/util.py"
run "$PY" build --locked "$PROJ/main.py" -o "$WORK/app_tampered"
expect_fail "source edit invalidates the lock" "pyadamas.lock is out of date"
expect_grep "source edit diff shows old -> new tree hash" "$ERR" "tree sha256:$OLD_HASH ->"
run "$PY" resolve "$PROJ"
NEW_HASH=$(sed -nE 's/^pkg libfoo@0\.2\.0 .*tree=sha256:([0-9a-f]{64})$/\1/p' "$LOCK")
if [ -n "$NEW_HASH" ] && [ "$NEW_HASH" != "$OLD_HASH" ]; then
    ok "source edit changes the tree hash"
else
    bad "source edit changes the tree hash" "old=$OLD_HASH new=$NEW_HASH"
fi
run "$PY" build --locked "$PROJ/main.py" -o "$WORK/app_updated"
expect_exit_ok "locked: updated lock builds again"
run "$WORK/app_updated"
expect_ok "locked: updated binary still prints foo bar 42" "foo bar 42"

# 依赖目录不存在
sed 's|libfoo = { path = "deps/libfoo" }|libfoo = { path = "deps/nope" }|' \
    "$WORK/good.toml" >"$PROJ/pyadamas.toml"
run "$PY" resolve "$PROJ"
expect_fail "missing dependency directory fails" "does not exist"
expect_grep "missing dependency suggests what P0 needs" "$ERR" "P0 dependencies must be directories"

# 依赖没有清单
cp "$WORK/good.toml" "$PROJ/pyadamas.toml"
mkdir -p "$PROJ/deps/nolist"
sed '/libbar = { path = "deps\/libbar" }/a nolist = { path = "deps/nolist" }' \
    "$WORK/good.toml" >"$PROJ/pyadamas.toml"
run "$PY" resolve "$PROJ"
expect_fail "dependency without manifest fails" "has no pyadamas.toml"
cp "$WORK/good.toml" "$PROJ/pyadamas.toml"
rmdir "$PROJ/deps/nolist" 2>/dev/null

# 映射指向不存在的模块
sed 's|bar = { package = "libbar", module = "bar" }|bar = { package = "libbar", module = "ghost" }|' \
    "$WORK/good.toml" >"$PROJ/pyadamas.toml"
run "$PY" resolve "$PROJ"
expect_fail "missing module file fails" "was not found in package 'libbar'"
cp "$WORK/good.toml" "$PROJ/pyadamas.toml"

# 保留名（stdlib）不能被包占用
mkdir -p "$WORK/reserved"
cat >"$WORK/reserved/pyadamas.toml" <<'EOF'
[package]
name = "reserved"
version = "0.1.0"

[dependencies]
libfoo = { path = "../proj/deps/libfoo" }

[modules]
math = { package = "libfoo", module = "foo" }
EOF
run "$PY" resolve "$WORK/reserved"
expect_fail "stdlib module name is reserved" "reserved by the standard library"
expect_grep "reserved name error says aliases are P2" "$ERR" "aliases are P2"

# registry / version / git 依赖：P0 明确拒绝
mkdir -p "$WORK/registry"
cat >"$WORK/registry/pyadamas.toml" <<'EOF'
[package]
name = "registry_user"
version = "0.1.0"

[dependencies]
libreg = { version = "1.0" }
EOF
run "$PY" resolve "$WORK/registry"
expect_fail "version dependency is rejected" "P0 only supports path dependencies"
cat >"$WORK/registry/pyadamas.toml" <<'EOF'
[package]
name = "registry_user"
version = "0.1.0"

[dependencies]
libgit = { git = "https://example.invalid/lib.git" }
EOF
run "$PY" resolve "$WORK/registry"
expect_fail "git dependency is rejected" "P0 only supports path dependencies"

# [aliases] 非空：P2
mkdir -p "$WORK/aliases"
cat >"$WORK/aliases/pyadamas.toml" <<'EOF'
[package]
name = "alias_user"
version = "0.1.0"

[dependencies]
libfoo = { path = "../proj/deps/libfoo" }

[aliases]
json2 = { package = "libfoo", version = "0.2.0" }
EOF
run "$PY" resolve "$WORK/aliases"
expect_fail "non-empty [aliases] is rejected as P2" "[aliases] is P2"

# 未识别的键 / 语法错误要指到行列
mkdir -p "$WORK/bad"
cat >"$WORK/bad/pyadamas.toml" <<'EOF'
[package]
name = "bad"
version = "0.1.0"
bogus = "x"
EOF
run "$PY" resolve "$WORK/bad"
expect_fail "unknown key is rejected" "unknown key 'bogus'"
expect_grep "unknown key points at line:col" "$ERR" "pyadamas.toml:4:1:"
cat >"$WORK/bad/pyadamas.toml" <<'EOF'
[package]
name = "bad"
version = "0.1.0
EOF
run "$PY" resolve "$WORK/bad"
expect_fail "unterminated string is rejected" "unterminated string"
expect_grep "syntax error points at line:col" "$ERR" "pyadamas.toml:3:11:"

# 循环 path 依赖
mkdir -p "$WORK/cycle/a" "$WORK/cycle/b"
cat >"$WORK/cycle/pyadamas.toml" <<'EOF'
[package]
name = "cycle_root"
version = "0.1.0"

[dependencies]
cyc_a = { path = "a" }
EOF
cat >"$WORK/cycle/a/pyadamas.toml" <<'EOF'
[package]
name = "cyc_a"
version = "0.1.0"

[dependencies]
cyc_b = { path = "../b" }
EOF
cat >"$WORK/cycle/b/pyadamas.toml" <<'EOF'
[package]
name = "cyc_b"
version = "0.1.0"

[dependencies]
cyc_a = { path = "../a" }
EOF
run "$PY" resolve "$WORK/cycle"
expect_fail "path dependency cycle fails" "path dependency cycle:"

# 没有依赖的项目：锁只有 root 行，派生出的 map 为空，构建照常。
mkdir -p "$WORK/nodeps"
cat >"$WORK/nodeps/pyadamas.toml" <<'EOF'
[package]
name = "nodeps"
version = "0.1.0"
EOF
cat >"$WORK/nodeps/main.py" <<'EOF'
print("nodeps ok")
EOF
run "$PY" resolve "$WORK/nodeps"
expect_ok "no dependencies: resolve writes a root-only lock" "0 packages"
run "$PY" build --locked "$WORK/nodeps/main.py" -o "$WORK/nodeps_app"
expect_exit_ok "no dependencies: --locked build"
run "$WORK/nodeps_app"
expect_ok "no dependencies: binary runs" "nodeps ok"

# 手改锁：绝对路径必须被拒绝（可移植性是锁的硬性质）
cp "$LOCK" "$WORK/lock.abs"
sed -i 's|path="deps/libfoo"|path="/tmp/libfoo"|' "$WORK/lock.abs"
cp "$WORK/lock.abs" "$PROJ/pyadamas.lock"
run "$PY" build --locked "$PROJ/main.py" -o "$WORK/app_abs"
expect_fail "locked: lock with absolute path is rejected" "must be a relative path"

echo
if [ "$fail" -ne 0 ]; then
    echo "pkg: FAILED ($checks checks)"
    exit 1
fi
echo "pkg ok ($checks checks)"
