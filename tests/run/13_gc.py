# 循环收集器：对象环、闭包垃圾、unknown 容器，以及跨 GC 的存活图
shared: str = "same"

class Node:
    name: str
    def __init__(self, s: str) -> None:
        self.name = s
        self.next = None

i: int = 0
while i < 30000:
    a = Node(shared)
    b = Node(shared)
    a.next = b
    b.next = a
    i = i + 1
print(i)

def make(v: int):
    return lambda: v

keep = make(7)
j: int = 0
while j < 50000:
    junk = make(j)
    j = j + 1
print(keep())

l: list[str] = []
k: int = 0
while k < 5000:
    l.append("item" + str(k))
    k = k + 1

total: int = 0
for s in l:
    total = total + len(s)
print(total)

# 字典自环：值类型是 unknown，正常 RC 不解引用，靠 GC 清扫
j: int = 0
while j < 20000:
    dd = {}
    dd["self"] = dd
    j = j + 1
print(j)

# --- expect ---
# 30000
# 7
# 38890
# 20000
