#!/usr/bin/env bash
# abbench.sh —— A/B 性能对比：当前工作区 vs 指定 git 版本。
#
# 背景：优化工作的核心动作是「改一处 -> 量收益」，而这件事以前每次都要
# 手工建 worktree、手工写测量脚本、手工算中位数。几个坑反复踩：
#   1. 忘了 --rebuild，modcache 命中旧产物，量出完全相同的二进制；
#   2. 只测一次，被系统噪声带偏（曾把布局位移误判成 17% 回归）；
#   3. 先测完全部 A 再测全部 B，系统漂移被算进差值。
#
# 这个脚本把三件事固定下来：
#   - 在独立 worktree 里构建基线，绝不碰当前 build 目录；
#   - 交替测量（A,B,A,B...），抵消系统漂移；
#   - 报中位数而不是最小值，并给出 p25/median 两个分位。
#
# 用法：
#   tools/abbench.sh                    # 当前 vs HEAD
#   tools/abbench.sh 575d4eb            # 当前 vs 指定提交
#   tools/abbench.sh HEAD realistic     # 只跑 bench/realistic
#   tools/abbench.sh HEAD all 11        # 全部基准，11 轮
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1

BASE_REF=${1:-HEAD}
SUITE=${2:-all}
REPS=${3:-9}
CUR_BIN=${PYADAMAS_BIN:-$ROOT/build/release/pyadamas}

if [ ! -x "$CUR_BIN" ]; then
    echo "error: 当前编译器不存在: $CUR_BIN（先 cmake --build build/release）" >&2
    exit 1
fi

# 1) 基线：独立 worktree + 独立 build 目录。
WT=/tmp/pyadamas-ab-base
rm -rf "$WT"
echo "[1/4] 构建基线 $BASE_REF ..."
git worktree remove --force "$WT" 2>/dev/null
git worktree add --detach "$WT" "$BASE_REF" >/dev/null 2>&1 || {
    echo "error: 无法创建 worktree（$BASE_REF 存在吗？）" >&2; exit 1; }
cmake -S "$WT" -B "$WT/build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 || {
    echo "error: 基线 configure 失败" >&2; exit 1; }
cmake --build "$WT/build" -j"$(nproc)" >/dev/null 2>&1 || {
    echo "error: 基线构建失败" >&2; exit 1; }
BASE_BIN=$WT/build/pyadamas

# 2) 收集用例。
case "$SUITE" in
    realistic) CASES=$(ls bench/realistic/*.py) ;;
    classic)   CASES=$(ls bench/compare/*.py) ;;
    all)       CASES=$(ls bench/realistic/*.py bench/compare/*.py) ;;
    *)         CASES=$SUITE ;;
esac

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"; git worktree remove --force "$WT" 2>/dev/null' EXIT

# 3) 构建两侧二进制。**必须 --rebuild**：modcache 的键不含「编译器版本」，
#    换了编译器但源码没变时会命中旧产物，量出完全相同的二进制。
echo "[2/4] 编译用例 ..."
declare -A A_BIN B_BIN
for f in $CASES; do
    name=$(basename "$f" .py)
    "$BASE_BIN" build -O2 --rebuild "$f" -o "$OUT/a_$name" >/dev/null 2>&1 || {
        echo "  skip $name（基线编译失败）"; continue; }
    "$CUR_BIN" build -O2 --rebuild "$f" -o "$OUT/b_$name" >/dev/null 2>&1 || {
        echo "  skip $name（当前编译失败）"; continue; }
    A_BIN[$name]=$OUT/a_$name
    B_BIN[$name]=$OUT/b_$name
done

# 4) 交替测量。每轮先 A 后 B，把系统漂移平均到两侧。
echo "[3/4] 测量 $REPS 轮（交替）..."
for name in "${!A_BIN[@]}"; do
    : > "$OUT/ta_$name"
    : > "$OUT/tb_$name"
done
for rep in $(seq 1 "$REPS"); do
    for name in "${!A_BIN[@]}"; do
        s=$(date +%s%N)
        taskset -c 0 "${A_BIN[$name]}" >/dev/null 2>&1
        e=$(date +%s%N)
        echo $(( (e - s) / 1000 )) >> "$OUT/ta_$name"
        s=$(date +%s%N)
        taskset -c 0 "${B_BIN[$name]}" >/dev/null 2>&1
        e=$(date +%s%N)
        echo $(( (e - s) / 1000 )) >> "$OUT/tb_$name"
    done
done

# 5) 出表：中位数 + p25，单位 ms。
echo "[4/4] 结果（中位数，交替 $REPS 轮，taskset -c 0）"
echo ""
printf '%-22s %10s %10s %9s %9s\n' "用例" "基线" "当前" "变化" "p25变化"
printf '%-22s %10s %10s %9s %9s\n' "----" "----" "----" "----" "-------"
tot_a=0; tot_b=0
for name in $(printf '%s\n' "${!A_BIN[@]}" | sort); do
    read -r amed ap25 < <(sort -n "$OUT/ta_$name" | awk '{a[NR]=$1} END{print a[int((NR+1)/2)], a[int((NR+1)/4)]}')
    read -r bmed bp25 < <(sort -n "$OUT/tb_$name" | awk '{a[NR]=$1} END{print a[int((NR+1)/2)], a[int((NR+1)/4)]}')
    [ -z "$amed" ] && continue
    tot_a=$((tot_a + amed)); tot_b=$((tot_b + bmed))
    pct=$(awk -v a="$amed" -v b="$bmed" 'BEGIN{printf "%+.1f%%", (b-a)*100/a}')
    p25=$(awk -v a="$ap25" -v b="$bp25" 'BEGIN{printf "%+.1f%%", (b-a)*100/a}')
    printf '%-22s %9.2fms %9.2fms %9s %9s\n' "$name" \
        "$(awk -v v="$amed" 'BEGIN{print v/1000}')" \
        "$(awk -v v="$bmed" 'BEGIN{print v/1000}')" "$pct" "$p25"
done
echo ""
awk -v a="$tot_a" -v b="$tot_b" 'BEGIN{printf "总计: %.2fms -> %.2fms  %+.1f%%\n", a/1000, b/1000, (b-a)*100/a}'
