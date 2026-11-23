#!/bin/bash
# opt_stats.sh —— 优化 pass 有效性审计（依赖 PYADAMAS_OPT_STATS=1 的指纹统计）
#
# 用法：tools/opt_stats.sh [--quick] [--gates-only]
#   --quick       只跑 12 个基准（不跑 tests/run 全量）
#   --gates-only  只看门控 pass（跳过默认 phases 的表格？保留，当前仅作说明）
#
# 默认对 12 个基准 + tests/run/ 全量编译两次：
#   phase 1 = 默认配置（loop_opt=0，inline_opt=off）
#   phase 2 = 全部门控打开（PYADAMAS_LOOP_OPT=7 PYADAMAS_INLINE_OPT=1）
# 汇总成表；「0 命中且不在门控白名单里」的 pass 高亮为 SUSPICIOUS 并以退出码 1 结束。
#
# 门控白名单（默认关是既定策略，0 命中不算故障）：
#   self_unroll / dedup_block_loads / cse#2 / thread_jump_blocks / version_bounds_affine
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${PYADAMAS_BIN:-$ROOT/build/lead/pyadamas}
MODE=${1:-}
QUICK=0
[ "$MODE" = "--quick" ] && QUICK=1

if [ ! -x "$BIN" ]; then
  echo "opt_stats: 找不到可执行文件 $BIN（先 cmake --build build/lead -j4）" >&2
  exit 2
fi

TMP=$(mktemp -d /tmp/optstats.XXXXXX)
trap 'rm -rf "$TMP"' EXIT

GATED="self_unroll dedup_block_loads cse#2 thread_jump_blocks version_bounds_affine"

FILES=()
for f in "$ROOT"/bench/compare/*.py; do FILES+=("$f"); done
if [ "$QUICK" -eq 0 ]; then
  for f in "$ROOT"/tests/run/*.py; do FILES+=("$f"); done
fi

# phase <name> <extra-env...>：编译 FILES，聚合 pass= 行到 $TMP/<name>.agg（pass changed funcs）
run_phase() {
  name="$1"; shift
  : > "$TMP/$name.agg"
  : > "$TMP/$name.seen"
  i=0
  for f in "${FILES[@]}"; do
    i=$((i + 1))
    out="$TMP/$name.$i.bin"
    env PYADAMAS_OPT_STATS=1 "$@" "$BIN" build --rebuild -O2 -o "$out" "$f" 2>&1 \
      | grep -E "^pass=" >> "$TMP/$name.lines" || true
    rm -f "$out"
  done
  python3 - "$name" "$TMP/$name.lines" "$TMP/$name.agg" "$TMP/$name.seen" << 'PYEOF'
import sys
name, lines_path, agg_path, seen_path = sys.argv[1:5]
agg = {}   # pass -> [changed, funcs]
seen = set()
try:
    lines = open(lines_path).read().splitlines()
except FileNotFoundError:
    lines = []
for ln in lines:
    parts = dict(p.split("=", 1) for p in ln.split() if "=" in p)
    if "pass" not in parts:
        continue
    nm = parts["pass"]
    ch = int(parts.get("changed", 0))
    fu = int(parts.get("funcs", 0))
    seen.add(nm)
    a = agg.setdefault(nm, [0, 0])
    a[0] += ch
    a[1] += fu
with open(agg_path, "w") as f:
    for nm in sorted(agg):
        f.write("%s %d %d\n" % (nm, agg[nm][0], agg[nm][1]))
with open(seen_path, "w") as f:
    for nm in sorted(seen):
        f.write(nm + "\n")
PYEOF
}

echo "== phase 1: 默认配置（$(echo "${#FILES[@]}") 个文件）==" >&2
: > "$TMP/default.lines"
run_phase default
echo "== phase 2: 全部门控打开（loop_opt=7, inline_opt=1）==" >&2
: > "$TMP/gated.lines"
run_phase gated PYADAMAS_LOOP_OPT=7 PYADAMAS_INLINE_OPT=1

PYTHON_OUT=$(python3 - "$TMP" "$GATED" << 'PYEOF'
import sys
tmp, gated = sys.argv[1], set(sys.argv[2].split())

def load(name):
    d = {}
    try:
        for ln in open(tmp + "/" + name + ".agg"):
            nm, ch, fu = ln.split()[:3]
            d[nm] = (int(ch), int(fu))
    except FileNotFoundError:
        pass
    return d

def seen(name):
    try:
        return set(open(tmp + "/" + name + ".seen").read().split())
    except FileNotFoundError:
        return set()

d_def, d_gat = load("default"), load("gated")
seen_def, seen_gat = seen("default"), seen("gated")
names = sorted(set(d_def) | set(d_gat) | set(gated))

print("")
print("| pass | 默认 changed/funcs | 门控全开 changed/funcs | 状态 |")
print("|---|---|---|---|")
suspicious = []
for nm in names:
    d = d_def.get(nm, (0, 0))
    g = d_gat.get(nm, (0, 0))
    present_def = nm in seen_def
    present_gat = nm in seen_gat
    if nm in gated:
        if not present_def and not present_gat:
            status = "GATED-未启用（两阶段都未调用）"
        elif g[0] == 0 and d[0] == 0:
            status = "GATED-0 命中（人工确认）"
        else:
            status = "ok"
    else:
        if not present_def and not present_gat:
            status = "SUSPICIOUS-从未调用"
            suspicious.append(nm)
        elif d[0] == 0 and g[0] == 0:
            status = "SUSPICIOUS-0 命中"
            suspicious.append(nm)
        else:
            status = "ok"
    print("| %s | %d/%d | %d/%d | %s |" % (nm, d[0], d[1], g[0], g[1], status))

print("")
if suspicious:
    print("可疑 pass（0 命中且不在门控白名单）：" + ", ".join(suspicious))
    print("exit=1")
else:
    print("没有可疑 pass：所有非门控 pass 至少命中一次。")
    print("exit=0")
PYEOF
)

echo "$PYTHON_OUT" | grep -v "^exit="
EXIT_CODE=$(echo "$PYTHON_OUT" | grep "^exit=" | cut -d= -f2)
[ -z "${EXIT_CODE:-}" ] && EXIT_CODE=2

if [ "$EXIT_CODE" = "1" ]; then
  echo "" >&2
  echo "opt_stats: 有可疑 pass（见上表 SUSPICIOUS）" >&2
fi
exit "$EXIT_CODE"
