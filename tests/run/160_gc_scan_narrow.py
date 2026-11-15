# GC 临时区扫描收窄（技术债 #1）：大量非指针局部 + unknown 借用引用。
#
# 这个帧里同时有：
#   - 一串 int/float/bool 临时值：永远不可能装堆指针，收窄后一个字节都不扫；
#   - unknown 借用引用 v：只能靠保守扫描兜住，位图必须覆盖它；
#   - id() 得到的「恰好等于堆地址的整数」：整段扫描会把它当根，把整片垃圾
#     环钉在堆上（误报保留）；位图收窄后这些字不再扫描。
# 循环里持续分配，触发多轮 minor / major。输出与收窄前逐字节一致。
#
# 误报保留的实测（PYADAMAS_GC_STATS=1）：收窄前 86 个 / 7568 字节只在保守
# 根下存活，收窄后 0。
# 回退口径：PYADAMAS_GC_CONSERVATIVE=1 时退回整段扫描，输出仍必须相同。

class Node:
    name: str
    def __init__(self, s: str) -> None:
        self.name = s
        self.next = None


def cycle_addr() -> int:
    # 建一个只有环内引用的小环，返回其中一个节点的机器地址（id 的裸机器
    # 字）。函数返回后环只剩内部边，是收集器的合法目标。
    a = Node("a")
    b = Node("b")
    a.next = b
    b.next = a
    return id(a)


def churn(n: int) -> int:
    i: int = 0
    total: int = 0
    while i < n:
        junk = [i, i + 1, i + 2]
        total = total + len(junk)
        i = i + 1
    return total


def mixed(flag: int, n: int) -> int:
    box = {}
    if flag > 0:
        box["k"] = "abc"
    else:
        box["k"] = 7
    v = box["k"]              # 借来的 unknown 引用，跨 GC 必须存活
    # 下面这一整串都是标量临时值：收窄后它们不进临时区扫描。
    p: int = 3
    q: int = p * 7 - 2
    r: float = 1.25
    t: float = r * 3.5 + 0.5
    b1: bool = (q % 5) == 0
    b2: bool = (q % 3) == 0
    u: int = p ^ q
    # 两个地址整数活在临时区里跨过 churn 的收集点（id 的值不影响输出）。
    addr = cycle_addr() + (churn(n) + cycle_addr())
    total: int = 0
    if b1:
        total = total + 1
    if b2:
        total = total + 2
    if u > 0:
        total = total + (u & 255)
    if t > 2.0:
        total = total + 4
    if v == "abc":
        total = total + 100
    elif v == 7:
        total = total + 200
    if addr > 0:
        total = total + 8
    return total


acc: int = 0
i: int = 0
while i < 30:
    acc = acc + mixed(i % 2, 50000)
    i = i + 1
print(acc)

# --- expect ---
# 5340
