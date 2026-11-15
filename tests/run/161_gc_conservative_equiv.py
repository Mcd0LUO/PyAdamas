# 回退开关不改变语义：PYADAMAS_GC_CONSERVATIVE=1 必须完全退回整段保守
# 扫描，这个程序的输出必须逐字节相同。验证方式（详见 docs/runtime.md）：
#   ./build/zgc/pyadamas run --rebuild -o /tmp/161 tests/run/161_gc_conservative_equiv.py
#   /tmp/161                            与
#   PYADAMAS_GC_CONSERVATIVE=1 /tmp/161 输出一致
# 覆盖：unknown 借用引用跨 GC、环垃圾清扫、容器 churn、多轮 minor / major。

class Node:
    name: str
    def __init__(self, s: str) -> None:
        self.name = s
        self.next = None


def make_ring(n: int) -> int:
    first = Node("r0")
    cur = first
    i: int = 1
    while i < n:
        nxt = Node("r" + str(i))
        cur.next = nxt
        cur = nxt
        i = i + 1
    cur.next = first     # 环：RC 收不了，只能靠收集器清扫
    return len(first.name)


def churn(flag: int, n: int) -> int:
    d = {}
    if flag > 0:
        d["k"] = list(range(16))
    else:
        d["k"] = "payload"
    v = d["k"]           # 借来的 unknown 引用，跨 GC 存活
    total: int = 0
    i: int = 0
    while i < n:
        junk = [i, str(i), i + 0.5]
        if v == "payload":
            total = total + 1
        elif len(v) == 16:
            total = total + 2
        i = i + 1
    return total


acc: int = 0
i: int = 0
while i < 30:
    acc = acc + make_ring(200)
    acc = acc + churn(i % 2, 20000)
    i = i + 1
print(acc)

# --- expect ---
# 900060
