#!/usr/bin/env bash
# 并发模块缓存回归：两个进程同时 --rebuild 编译 hashlib/hmac，
# 缓存写入必须是原子的，不能读到截断的 mod.s/mod.o/manifest。
set -u
BUILD=${1:?build dir}
PY=${2:?pyadamas binary}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1

for i in $(seq 1 5); do
    "$PY" build --rebuild tests/run/115_hashlib.py -o "$BUILD/cache_race_h$i" \
        >"$BUILD/cache_race_h$i.log" 2>&1 &
    p1=$!
    "$PY" build --rebuild tests/run/116_hmac.py -o "$BUILD/cache_race_m$i" \
        >"$BUILD/cache_race_m$i.log" 2>&1 &
    p2=$!
    if ! wait "$p1"; then
        echo "hashlib build failed on round $i"
        sed -n '1,8p' "$BUILD/cache_race_h$i.log"
        exit 1
    fi
    if ! wait "$p2"; then
        echo "hmac build failed on round $i"
        sed -n '1,8p' "$BUILD/cache_race_m$i.log"
        exit 1
    fi
done
echo "cache race ok"

