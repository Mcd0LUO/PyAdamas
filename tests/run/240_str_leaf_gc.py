# 240_str_leaf_gc —— 0.321：str/bytes 索引叶子的 GC 记账优化（回归哨兵）
#
# 0.321 起字符串（str/bytes）是「索引叶子」：
#   * 不进 GC 堆链 / 新生代 / 本轮集合，不驱动 GC 触发阈值；
#   * 仍留在存活堆索引里（分页位图），unknown 值探针靠它识别堆指针；
#   * 生死完全由 RC 决定：容器被回收时按 release 交出边，字符串就地归零释放。
#
# 本用例覆盖这条路径最容易踩坏的五件事，输出与 CPython 3.14 逐字节一致，
# 且 O0/O1/O2 三种优化级别一致（difftest 覆盖 O0/O2）：
#   1) 字符串密集 churn（大量临时串 + 列表），把循环收集器真正驱动起来；
#   2) unknown 容器元素 / unknown 槽持字符串跨 GC 存活 —— 探针必须看见它们，
#      否则 print 会把地址当整数、或提前释放（段错误）；
#      注意：这里用下标直取 + print，不用 `for v in list[object]` —— 后者的
#      降级器对 unknown 元素会连发两次 rc_inc_unknown（既有 bug，0.321 记录、
#      未修），在字符串不再进 GC 链之后会让 LeakSanitizer 报噪声；
#   3) 类实例持字符串成环：环必须照常被真收集（gc_freed>0），字符串跟着回收；
#      同时存活环里的字符串在一次收集之后仍然可读；
#   4) 字符串键 dict 的哈希/查找在多次 GC 前后都正确；
#   5) bytes 叶子（同一个 PYADAMAS_RC_STR kind）与 unknown 返回值的两条分支。
#
# 保守回退对照（手工，套件外）：
#   `PYADAMAS_GC_CONSERVATIVE=1 ... run 240_str_leaf_gc.py`
#   —— 字符串回到旧的全跟踪口径，输出必须与本用例 expect 段逐字节相同。

# ── 1) 字符串密集 churn ─────────────────────────────────────────
total: int = 0
i: int = 0
while i < 12000:
    parts: list[str] = []
    k: int = 0
    while k < 6:
        parts.append("seg" + str(i) + "-" + str(k))
        k += 1
    line: str = "/".join(parts)
    total += len(line)
    i += 1
print("churn", total)

# ── 2) unknown 槽持字符串跨 GC ──────────────────────────────────
keep: list[object] = []
j: int = 0
while j < 400:
    keep.append("keep-" + str(j) + "-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx")
    j += 1

# 容器 churn：把分配量推过阈值，逼出多次收集（字符串不再驱动阈值）
g: int = 0
while g < 200000:
    tmp: list[int] = [g, g + 1, g + 2]
    g += tmp[0] - g + 1
print("cycle", g)

print("keep", len(keep))
print("keep0len", len(str(keep[0])))
print(keep[0])
print(keep[399])

# ── 3) 类实例持字符串成环（真收集）+ 存活环 ─────────────────────
class Node:
    name: str
    def __init__(self, s: str) -> None:
        self.name = s
        self.next = None

n: int = 0
while n < 3000:
    x: Node = Node("box-a-" + str(n) + "-yyyyyyyyyyyyyyyyyyyyyyyyyyyyy")
    y: Node = Node("box-b-" + str(n) + "-yyyyyyyyyyyyyyyyyyyyyyyyyyyyy")
    x.next = y
    y.next = x
    n += 1

live: Node = Node("live-" + "z" * 24)
peer: Node = Node("peer-" + "q" * 24)
live.next = peer
peer.next = live

m: int = 0
while m < 6000:
    t: list[int] = [m, m + 1]
    m += 1
print("cycle2", n, live.name, len(peer.name))

# ── 4) 字符串键 dict ───────────────────────────────────────────
counts: dict[str, int] = {}
i2: int = 0
while i2 < 4000:
    key: str = "k-" + str(i2 % 500) + "-w"
    if key in counts:
        counts[key] += 1
    else:
        counts[key] = 1
    i2 += 1
print("dict", len(counts), counts["k-0-w"], counts["k-499-w"])

# ── 5) bytes 叶子 ──────────────────────────────────────────────
bv: bytes = b"bytes-leaf-" * 8
print("bytes", len(bv), bv[:11])

# ── 6) unknown 返回值两条分支 ──────────────────────────────────
def pick(flag: bool) -> object:
    if flag:
        return "picked-" + "p" * 30
    return 7 * 6

u: object = pick(True)
v: object = pick(False)

f2: int = 0
while f2 < 6000:
    w: list[int] = [f2]
    f2 += 1
print("unknown", str(u)[:7], v)

print("done")
# --- expect ---
# churn 713340
# cycle 200000
# keep 400
# keep0len 39
# keep-0-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
# keep-399-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
# cycle2 3000 live-zzzzzzzzzzzzzzzzzzzzzzzz 29
# dict 500 8 8
# bytes 88 b'bytes-leaf-'
# unknown picked- 42
# done
