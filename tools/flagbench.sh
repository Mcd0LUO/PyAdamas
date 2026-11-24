#!/usr/bin/env bash
# flagbench.sh —— 同一个编译器、同一个源码，只切换一个环境变量做 A/B（0.481 收编）。
#
# 为什么需要它：实验开关的「默认化决策」必须用同一编译器、同窗、打乱顺序、奇偶轮交换、
# BURST 连跑取平均、15 轮中位/p25 来量（工具 /tmp 里的临时脚本用过三次后收编进仓库，
# 见 docs/experiment-switches.md §2）。它只切一个环境变量，两侧独立 PYADAMAS_CACHE_DIR +
# --rebuild（缓存键含开关，混用会拿到另一侧产物），并先比对两侧输出必须逐字节相同。
# 不碰 build/release（0.271 语料 oracle）。
#
# 用法：tools/flagbench.sh <ENV_NAME> <suite|files> [REPS] [BURST]
#   suite: realistic | classic | all；也可以直接给一串 .py 路径
#   例：BURST=20 bash tools/flagbench.sh PYADAMAS_BORROW_OPT bench/daily/*.py 15
# 环境：PYADAMAS_BIN（默认 build/lead/pyadamas）、BURST（默认 5）
# 两侧分别用独立 PYADAMAS_CACHE_DIR + --rebuild 编译（缓存键含门控开关），
# 交替测量（每轮打乱顺序 + 奇偶轮交换 A/B 先后），报中位数与 p25。
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT" || exit 1
ENVN=${1:?env name}
SUITE=${2:-realistic}
REPS=${3:-15}
BURST=${BURST:-5}
CUR_BIN=${PYADAMAS_BIN:-$ROOT/build/lead/pyadamas}
PIN_CPU=$(awk '/Cpus_allowed_list/ {print $2}' /proc/self/status | tr ',' '\n' | head -1 | cut -d- -f1)
PIN_CPU=${PIN_CPU:-0}
case "$SUITE" in
    realistic) CASES=$(ls bench/realistic/*.py) ;;
    classic)   CASES=$(ls bench/compare/*.py) ;;
    all)       CASES=$(ls bench/realistic/*.py bench/compare/*.py) ;;
    *)         CASES=$SUITE ;;
esac
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
declare -A A_BIN B_BIN
for f in $CASES; do
    name=$(basename "$f" .py)
    [ "$name" = run_compare ] && continue
    env -u "$ENVN" PYADAMAS_CACHE_DIR="$OUT/ca" "$CUR_BIN" build -O2 --rebuild "$f" -o "$OUT/a_$name" >/dev/null 2>&1 || { echo "  skip $name (off)"; continue; }
    env "$ENVN=1" PYADAMAS_CACHE_DIR="$OUT/cb" "$CUR_BIN" build -O2 --rebuild "$f" -o "$OUT/b_$name" >/dev/null 2>&1 || { echo "  skip $name (on)"; continue; }
    A_BIN[$name]=$OUT/a_$name
    B_BIN[$name]=$OUT/b_$name
done
# 输出一致性（两侧都跑，必须逐字节相同）
for name in "${!A_BIN[@]}"; do
    oa=$(taskset -c "$PIN_CPU" "${A_BIN[$name]}" 2>&1)
    ob=$(taskset -c "$PIN_CPU" "${B_BIN[$name]}" 2>&1)
    [ "$oa" = "$ob" ] || echo "  !! OUTPUT DIFF $name"
done
for name in "${!A_BIN[@]}"; do : > "$OUT/ta_$name"; : > "$OUT/tb_$name"; done
mapfile -t ALL < <(printf '%s\n' "${!A_BIN[@]}")
for rep in $(seq 1 "$REPS"); do
    mapfile -t order < <(printf '%s\n' "${ALL[@]}" | sort -R)
    for name in "${order[@]}"; do
        for side in $( (( rep % 2 )) && echo "a b" || echo "b a"); do
            if [ "$side" = a ]; then bin=${A_BIN[$name]}; tf=$OUT/ta_$name
            else bin=${B_BIN[$name]}; tf=$OUT/tb_$name; fi
            s=$(date +%s%N)
            for ((k = 0; k < BURST; k++)); do taskset -c "$PIN_CPU" "$bin" >/dev/null 2>&1; done
            e=$(date +%s%N)
            echo $(( (e - s) / 1000 / BURST )) >> "$tf"
        done
    done
done
printf '%-22s %10s %10s %9s %9s\n' "case" "off" "on" "delta" "p25"
tot_a=0; tot_b=0
for name in $(printf '%s\n' "${!A_BIN[@]}" | sort); do
    read -r amed ap25 < <(sort -n "$OUT/ta_$name" | awk '{a[NR]=$1} END{i=int((NR+1)/4); if(i<1)i=1; print a[int((NR+1)/2)], a[i]}')
    read -r bmed bp25 < <(sort -n "$OUT/tb_$name" | awk '{a[NR]=$1} END{i=int((NR+1)/4); if(i<1)i=1; print a[int((NR+1)/2)], a[i]}')
    [ -z "$amed" ] && continue
    tot_a=$((tot_a + amed)); tot_b=$((tot_b + bmed))
    pct=$(awk -v a="$amed" -v b="$bmed" 'BEGIN{printf "%+.1f%%", (b-a)*100/a}')
    p25=$(awk -v a="$ap25" -v b="$bp25" 'BEGIN{printf "%+.1f%%", (b-a)*100/a}')
    printf '%-22s %9.2fms %9.2fms %9s %9s\n' "$name" \
        "$(awk -v v="$amed" 'BEGIN{print v/1000}')" \
        "$(awk -v v="$bmed" 'BEGIN{print v/1000}')" "$pct" "$p25"
done
awk -v a="$tot_a" -v b="$tot_b" 'BEGIN{printf "total: %.2fms -> %.2fms  %+.1f%%\n", a/1000, b/1000, (b-a)*100/a}'
