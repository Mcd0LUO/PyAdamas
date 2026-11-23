#!/usr/bin/env bash
# pyadamas 测试套件：正例 / 反例 / 运行 / 未建模 / 多模块 + 缓存。
# 用法：run_tests.sh <build-dir> <pyadamas-可执行文件>
#
# 敏捷模式：
#   PYADAMAS_TEST_FILTER='3[0-4]*' PYADAMAS_TEST_JOBS=8 bash tests/run_tests.sh build/dev build/dev/pyadamas
#   PYADAMAS_TEST_FILTER 按 basename 匹配（shell glob），只跑选中的用例。
#   PYADAMAS_TEST_JOBS 控制并行度，默认 nproc。
#
# 内存检查模式：
#   PYADAMAS_TEST_ASAN=1 bash tests/run_tests.sh build/dev build/dev/pyadamas
#   生成的程序带 AddressSanitizer 跑。RC/GC 类 bug 在 IR 层面看不出问题，
#   只有内存检查器能抓到「谁第一个读已释放内存」。慢 3-5 倍，日常不用，
#   改动触及 RC/GC/lowering 时跑一遍。
set -u

BUILD=${1:?build dir}
PYADAMAS=${2:?pyadamas binary}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1

JOBS=${PYADAMAS_TEST_JOBS:-$(nproc 2>/dev/null || echo 4)}
FILTER=${PYADAMAS_TEST_FILTER:-}
VERIFY_ENV=""
ASAN=${PYADAMAS_TEST_ASAN:-0}
# IR 校验模式：每个用例编译时打开 PYADAMAS_VERIFY_IR，任何不变量违规都会
# 出现在 stderr 里（编译仍会成功）。用来在改 pass 之后确认没有把 IR 改坏。
# 基线是零违规，所以这里一旦有输出就是回归。
VERIFY=${PYADAMAS_TEST_VERIFY:-0}
# sanitizer 模式下的额外 build 参数与运行环境。
if [ "$ASAN" = 1 ]; then
    BUILD_FLAGS="--asan"
    # 默认关 leak 检测：GC 程序里「留到进程退出」的对象很多，会淹没真正的
    # use-after-free。要查泄漏时显式 ASAN_OPTIONS=detect_leaks=1。
    RUN_ENV="ASAN_OPTIONS=detect_leaks=0:halt_on_error=1"
else
    BUILD_FLAGS=""
    RUN_ENV=""
fi
if [ "$VERIFY" = 1 ]; then
    VERIFY_ENV="PYADAMAS_VERIFY_IR=1"
fi
match() {
    [ -z "$FILTER" ] && return 0
    case "$(basename "$1")" in
        $FILTER) return 0 ;;
        *) return 1 ;;
    esac
}

# 依赖 OpenBLAS 的用例；库不存在时跳过而不是判失败。
HAVE_OPENBLAS=0
if find /usr/lib /lib -name 'libopenblas.so*' -print -quit 2>/dev/null | grep -q .; then
    HAVE_OPENBLAS=1
fi
need_blas() {
    case "$(basename "$1")" in
        99_cblas.py | 100_linalg.py) [ "$HAVE_OPENBLAS" = 1 ] ;;
        *) return 0 ;;
    esac
}

fail=0
prog="$BUILD/prog.tmp"
out="$BUILD/out.txt"
exp="$BUILD/exp.txt"

for f in tests/*.py; do
    match "$f" || continue
    name=$(basename "$f")
    printf '  %-28s ' "$name"
    if "$PYADAMAS" check "$f" > "$BUILD/check.txt" 2>&1; then
        head -1 "$BUILD/check.txt"
    else
        printf 'FAILED\n'
        sed -n '1,6p' "$BUILD/check.txt"
        fail=1
    fi
done

# 用例级编译开关（与 run_test 同口径）：fail/strict/unsupported 循环也认
# 文件头的 type-tags / lang-gaps / loop-opt 指令（门控负面用例需要）。
test_extra_env() {
    local extra=""
    if sed -n '1,8p' "$1" | grep -q '^# pyadamas: type-tags'; then
        extra="PYADAMAS_TYPE_TAGS=1"
    fi
    if sed -n '1,8p' "$1" | grep -q '^# pyadamas: lang-gaps'; then
        extra="PYADAMAS_LANG_GAPS=1"
    fi
    # 整数循环优化（仿射 BCE / 跳转线程化，默认关）：带这个指令的用例
    # 必须打开门控编译，否则门控里的错代码一次都跑不到。
    if sed -n '1,8p' "$1" | grep -q '^# pyadamas: loop-opt'; then
        extra="$extra PYADAMAS_LOOP_OPT=1"
    fi
    # 特化 SIMD（0.384 起默认关，只作对照）：守向量化路径的用例必须显式开，
    # 否则测的是标量 fallback，路径里的错代码一次都跑不到。
    if sed -n '1,8p' "$1" | grep -q '^# pyadamas: simd'; then
        extra="$extra PYADAMAS_SIMD=1"
    fi
    # 通用向量化（0.384 起默认开）：负例/边界用例显式开，别让默认值变化把
    # 它们悄悄降级成标量测试。
    if sed -n '1,8p' "$1" | grep -q '^# pyadamas: vect'; then
        extra="$extra PYADAMAS_VECT=1"
    fi
    printf '%s' "$extra"
}

for f in tests/fail/*.py; do
    match "$f" || continue
    name=$(basename "$f")
    want=$(head -1 "$f" | sed -n 's/^# expect: //p')
    got=$(env $(test_extra_env "$f") "$PYADAMAS" check "$f" 2>&1 >/dev/null | head -1)
    printf '  %-28s ' "$name"
    if [ -z "$got" ]; then
        printf 'MISSED (expected: %s)\n' "$want"
        fail=1
    elif ! printf '%s' "$got" | grep -qF "$want"; then
        printf 'WRONG: %s\n' "$got"
        fail=1
    else
        printf 'rejected (%s)\n' "$want"
    fi
done

# ── 运行用例：并行编译 + 运行，结果按文件名顺序汇总 ──────────────
logdir="$BUILD/testlogs.$$"
rm -rf "$logdir"
mkdir -p "$logdir"

run_test() {
    local f=$1 name res extra
    name=$(basename "$f")
    res="$logdir/$name.res"
    local p="$logdir/$name.prog" o="$logdir/$name.out" e="$logdir/$name.exp"
    # 用例级编译开关：文件头出现「# pyadamas: type-tags」时以
    # PYADAMAS_TYPE_TAGS=1 编译运行（P2 运行期类型标签，默认关；这些用例
    # 的正确输出依赖标签，关着跑必然 OUTPUT DIFF）。
    extra=""
    if sed -n '1,8p' "$f" | grep -q '^# pyadamas: type-tags'; then
        extra="PYADAMAS_TYPE_TAGS=1"
    fi
    # P4 语言缺口（lang-gaps）：异常类当值/file 协议/traceback/dataclass。
    # 驱动侧 LANG_GAPS=1 会强制带上 TYPE_TAGS（CLASS tag 是表示基础）。
    if sed -n '1,8p' "$f" | grep -q '^# pyadamas: lang-gaps'; then
        extra="PYADAMAS_LANG_GAPS=1"
    fi
    # 整数循环优化（loop-opt，默认关）：仿射 BCE 的回归用例必须带门控编译。
    if sed -n '1,8p' "$f" | grep -q '^# pyadamas: loop-opt'; then
        extra="$extra PYADAMAS_LOOP_OPT=1"
    fi
    # 输出加大小上限：用例卡进死循环时它会一直写，曾经留下 19G 的单个
    # 日志文件（78_enumerate_zip.py.out），把 build 目录撑到 32G。
    # 截断后 diff 必然不等，用例照样报 OUTPUT DIFF，不影响判定。
    # run 子命令会 fork 出编译产物再执行；sanitizer 选项要靠环境变量传给
    # 那个子进程，所以套一层 env。
    if ! env $RUN_ENV $extra $VERIFY_ENV "$PYADAMAS" $BUILD_FLAGS run --rebuild -o "$p" "$f" 2>&1 | head -c 1048576 > "$o"; then
        { echo FAILED; sed -n '1,6p' "$o"; } > "$res"
        return
    fi
    sed -n '/^# --- expect ---$/,$p' "$f" | tail -n +2 | sed -E 's/^# ?//' > "$e"
    if diff -q "$e" "$o" > /dev/null 2>&1; then
        echo "ran ok" > "$res"
    else
        { echo "OUTPUT DIFF"; diff "$e" "$o" | head -12; } > "$res"
    fi
}

# 兜底清理：异常退出（Ctrl-C、崩溃）时也要把日志目录删掉，否则
# build/ 下会堆积 testlogs.<pid> 目录。
cleanup_logs() { rm -rf "$logdir"; }
trap cleanup_logs EXIT INT TERM

for f in tests/run/*.py; do
    match "$f" || continue
    need_blas "$f" || continue
    ( run_test "$f" ) &
    while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do
        wait -n || true
    done
done
wait

for f in tests/run/*.py; do
    match "$f" || continue
    name=$(basename "$f")
    printf '  %-28s ' "$name"
    if ! need_blas "$f"; then
        printf 'skipped (no OpenBLAS)\n'
        continue
    fi
    res="$logdir/$name.res"
    if [ -f "$res" ]; then
        first=$(head -1 "$res")
        printf '%s\n' "$first"
        tail -n +2 "$res"
        [ "$first" = "ran ok" ] || fail=1
    else
        printf 'MISSING RESULT\n'
        fail=1
    fi
done

for f in tests/unsupported/*.py; do
    match "$f" || continue
    name=$(basename "$f")
    want=$(head -1 "$f" | sed -n 's/^# unsupported: //p')
    got=$(env $(test_extra_env "$f") "$PYADAMAS" check -v "$f" 2>&1 >/dev/null | head -1)
    printf '  %-28s ' "$name"
    if ! printf '%s' "$got" | grep -qF "$want"; then
        printf 'NOT MARKED (expected: %s)\n' "$want"
        fail=1
    elif env $(test_extra_env "$f") "$PYADAMAS" build -o "$prog" "$f" > /dev/null 2>&1; then
        printf 'COMPILED ANYWAY\n'
        fail=1
    else
        printf 'parsed, refused by backend\n'
    fi
done

# ── 必须拒绝（错误在降级/构建期才出现，check 看不到）────────────
for f in tests/strict/*.py; do
    [ -e "$f" ] || continue
    match "$f" || continue
    name=$(basename "$f")
    want=$(head -1 "$f" | sed -n 's/^# expect: //p')
    printf '  %-28s ' "$name"
    got=$(env $(test_extra_env "$f") "$PYADAMAS" build -o "$prog" "$f" 2>&1 >/dev/null | head -1)
    if [ -z "$got" ]; then
        printf 'COMPILED (expected refusal: %s)\n' "$want"
        fail=1
    elif ! printf '%s' "$got" | grep -qF "$want"; then
        printf 'WRONG: %s\n' "$got"
        fail=1
    else
        printf 'rejected (%s)\n' "$want"
    fi
done

# ── 多模块：并行（各自独立目录，缓存互不影响）──────────────────
modlog="$BUILD/modlogs.$$"
rm -rf "$modlog"
mkdir -p "$modlog"

run_module() {
    local d=$1 name res
    name=$(basename "$d")
    res="$modlog/$name.res"
    local p="$modlog/$name.prog" o="$modlog/$name.out" e="$modlog/$name.exp" err="$modlog/$name.err"
    if ! env $(test_extra_env "$d/main.py") "$PYADAMAS" run --rebuild -o "$p" "$d/main.py" > "$o" 2>&1; then
        { echo FAILED; sed -n '1,8p' "$o"; } > "$res"
        return
    fi
    sed -n '/^# --- expect ---$/,$p' "$d/main.py" | tail -n +2 | sed -E 's/^# ?//' > "$e"
    if ! diff -q "$e" "$o" > /dev/null 2>&1; then
        { echo "OUTPUT DIFF"; diff "$e" "$o" | head -12; } > "$res"
        return
    fi
    if ! env $(test_extra_env "$d/main.py") "$PYADAMAS" run -v -o "$p" "$d/main.py" > /dev/null 2> "$err"; then
        echo "SECOND RUN FAILED" > "$res"
        return
    fi
    if ! grep -q "cached module" "$err"; then
        echo "CACHE MISS" > "$res"
        return
    fi
    if ! env $(test_extra_env "$d/main.py") "$PYADAMAS" check --imports "$d/main.py" > "$modlog/$name.chk" 2>&1; then
        { echo "CHECK --imports FAILED"; sed -n '1,6p' "$modlog/$name.chk"; } > "$res"
        return
    fi
    echo "ran ok" > "$res"
}

for d in tests/modules/*/; do
    match "${d}main.py" || continue
    ( run_module "$d" ) &
    while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do
        wait -n || true
    done
done
wait

for d in tests/modules/*/; do
    match "${d}main.py" || continue
    name=$(basename "$d")
    res="$modlog/$name.res"
    printf '  %-28s ' "module:$name"
    if [ -f "$res" ]; then
        first=$(head -1 "$res")
        printf '%s\n' "$first"
        tail -n +2 "$res"
        [ "$first" = "ran ok" ] || fail=1
    else
        printf 'MISSING RESULT\n'
        fail=1
    fi
done

# ── 缓存键正确性（全是「静默错误结果」类回归）──────────────────
#
# 守四件事：
#   * 切 --target 必须重编（否则拿另一个 ABI 的 mod.o 链接）；
#   * 老格式清单必须判失效（不能把 v1 当 v2 读，字段缺失 → 误命中）；
#   * toolchain 指纹失效必须生效（换编译器不能复用旧产物），且可覆盖/可命中；
#   * 接口哈希与缓存键都不许依赖绝对路径（换目录 = 同内容 = 同产物），
#     同源码拷到另一目录必须命中缓存、mod.pya 必须逐字节相同。
if match "cache:key"; then
    ck="$BUILD/cachekey"
    rm -rf "$ck"
    mkdir -p "$ck"
    cp -R tests/modules/chain "$ck/proj"
    cstore="$ck/store"
    ck_fail=0
    ck_run() { env PYADAMAS_CACHE_DIR="$cstore" "$PYADAMAS" "$@"; }
    printf '  %-28s ' "cache:key"
    # 1) 基线：首次构建成功，第二次必须命中缓存。
    if ! ck_run build -o "$ck/p1" "$ck/proj/main.py" > "$ck/b1.log" 2>&1; then
        echo "FAILED (first build)"; sed -n '1,6p' "$ck/b1.log"; ck_fail=1
    elif ! ck_run build -v -o "$ck/p1" "$ck/proj/main.py" 2> "$ck/b2.err"; then
        echo "FAILED (second build)"; sed -n '1,6p' "$ck/b2.err"; ck_fail=1
    elif ! grep -q 'cached module' "$ck/b2.err"; then
        echo "FAILED (second build did not hit cache)"; ck_fail=1
    fi
    # 2) 切目标 ABI 必须重编。win64 在本机只做交叉汇编验证，最终汇编/链接
    #    可能失败 —— 这里只看「有没有重新编译」，不看退出码。
    if [ "$ck_fail" = 0 ]; then
        ck_run build -v --target=x86_64-win64 -o "$ck/p2" "$ck/proj/main.py" 2> "$ck/b3.err" || true
        if ! grep -q 'building module' "$ck/b3.err"; then
            echo "FAILED (--target switch reused another ABI's cache)"; ck_fail=1
        fi
    fi
    # 3) 老格式清单（pyadamasmanifest 1）必须判失效。
    if [ "$ck_fail" = 0 ]; then
        ck_run build -o "$ck/p1" "$ck/proj/main.py" > /dev/null 2>&1 || true
        oldm=$(find "$cstore" -name manifest | head -1)
        if [ -n "$oldm" ]; then
            sed -i 's/^pyadamasmanifest 2$/pyadamasmanifest 1/' "$oldm"
            ck_run build -v -o "$ck/p1" "$ck/proj/main.py" 2> "$ck/b4.err" || true
            if ! grep -q 'building module' "$ck/b4.err"; then
                echo "FAILED (old manifest format accepted as new)"; ck_fail=1
            fi
        else
            echo "FAILED (no manifest written)"; ck_fail=1
        fi
    fi
    # 4) toolchain 覆盖口：换 id 必须失效；同 id 必须命中。
    if [ "$ck_fail" = 0 ]; then
        env PYADAMAS_CACHE_DIR="$cstore" PYADAMAS_TOOLCHAIN_ID=alt \
            "$PYADAMAS" build -v -o "$ck/p1" "$ck/proj/main.py" 2> "$ck/b5.err" || true
        if ! grep -q 'building module' "$ck/b5.err"; then
            echo "FAILED (toolchain change reused cache)"; ck_fail=1
        fi
    fi
    if [ "$ck_fail" = 0 ]; then
        env PYADAMAS_CACHE_DIR="$cstore" PYADAMAS_TOOLCHAIN_ID=alt \
            "$PYADAMAS" build -v -o "$ck/p1" "$ck/proj/main.py" 2> "$ck/b6.err" || true
        if ! grep -q 'cached module' "$ck/b6.err"; then
            echo "FAILED (same toolchain id did not hit cache)"; ck_fail=1
        fi
    fi
    # 5) 缓存键/接口哈希与绝对路径无关：同一份源码拷到另一目录，
    #    必须命中缓存（键含内容不含路径），且 mod.pya 逐字节相同。
    if [ "$ck_fail" = 0 ]; then
        # 步骤 4 把清单刷成了 alt 指纹；先恢复一次正常指纹的清单再比路径无关性。
        ck_run build -o "$ck/p1" "$ck/proj/main.py" > /dev/null 2>&1 || ck_fail=1
        find "$cstore" -name mod.pya -exec md5sum {} + 2>/dev/null | awk '{print $1}' | sort > "$ck/pya1"
        cp -R "$ck/proj" "$ck/proj2"
        ck_run build -v -o "$ck/p3" "$ck/proj2/main.py" 2> "$ck/b7.err" || ck_fail=1
        if [ "$ck_fail" = 0 ] && ! grep -q 'cached module' "$ck/b7.err"; then
            echo "FAILED (copy to another dir missed cache -> key depends on path)"; ck_fail=1
        fi
        if [ "$ck_fail" = 0 ]; then
            ck_run build --rebuild -o "$ck/p3" "$ck/proj2/main.py" > /dev/null 2>&1 || ck_fail=1
            find "$cstore" -name mod.pya -exec md5sum {} + 2>/dev/null | awk '{print $1}' | sort > "$ck/pya2"
            if ! diff -q "$ck/pya1" "$ck/pya2" > /dev/null 2>&1; then
                echo "FAILED (interface hash depends on absolute path)"; ck_fail=1
            fi
        fi
    fi
    rm -rf "$ck"
    [ "$ck_fail" = 0 ] && echo "ran ok" || fail=1
fi

# ── cdecl：头文件 → .cpya ──────────────────────────────────────
if match "cdecl"; then
    cdir="$BUILD/cdecl"
    rm -rf "$cdir"
    mkdir -p "$cdir"
    printf '  %-28s ' "cdecl:sample"
    if ! "$PYADAMAS" cdecl tests/cdecl/sample.h -o "$cdir/sample.cpya" \
            --link "tests/cdecl/sample.c" > "$cdir/gen.out" 2>&1; then
        printf 'GENERATE FAILED\n'
        sed -n '1,6p' "$cdir/gen.out"
        fail=1
    else
        sed -n '/^# --- expect ---$/,$p' tests/cdecl/main.py | tail -n +2 | \
            sed -E 's/^# ?//' > "$cdir/exp"
        if ! "$PYADAMAS" run --rebuild -I "$cdir" -o "$cdir/prog" \
                tests/cdecl/main.py > "$cdir/out" 2>&1; then
            printf 'RUN FAILED\n'
            sed -n '1,6p' "$cdir/out"
            fail=1
        elif diff -q "$cdir/exp" "$cdir/out" > /dev/null 2>&1; then
            printf 'ran ok\n'
        else
            printf 'OUTPUT DIFF\n'
            diff "$cdir/exp" "$cdir/out" | head -12
            fail=1
        fi
    fi
fi

# ── CFI：生成代码的栈展开规则 ─────────────────────────────────
#
# 探针用 libgcc 的 _Unwind_Backtrace 走一遍当前栈：它只认 .eh_frame，
# 不看 prologue、也不跟 rbp 链。生成代码的 CFI 一旦断了，深度会立刻
# 掉回 2（只数到第一个生成帧），比任何结构性检查都直接。
if match "cfi"; then
    fidir="$BUILD/cfi"
    rm -rf "$fidir"
    mkdir -p "$fidir"
    printf '  %-28s ' "cfi:unwind"
    if ! "$PYADAMAS" cdecl tests/cfi/probe.h -o "$fidir/probe.cpya" \
            --link "tests/cfi/probe.c" > "$fidir/gen.out" 2>&1; then
        printf 'GENERATE FAILED\n'
        sed -n '1,6p' "$fidir/gen.out"
        fail=1
    else
        sed -n '/^# --- expect ---$/,$p' tests/cfi/main.py | tail -n +2 | \
            sed -E 's/^# ?//' > "$fidir/exp"
        if ! "$PYADAMAS" run --rebuild -I "$fidir" -o "$fidir/prog" \
                tests/cfi/main.py > "$fidir/out" 2>&1; then
            printf 'RUN FAILED\n'
            sed -n '1,6p' "$fidir/out"
            fail=1
        elif diff -q "$fidir/exp" "$fidir/out" > /dev/null 2>&1; then
            printf 'ran ok\n'
        else
            printf 'OUTPUT DIFF\n'
            diff "$fidir/exp" "$fidir/out" | head -12
            fail=1
        fi
    fi
fi

rm -rf "$logdir" "$modlog"
rm -f "$prog" "$out" "$exp" "$BUILD/check.txt"
exit "$fail"
