# 增量标记正确性：a.next=b 中途写入 + 新对象引用老对象（插入屏障形态）。
class Box:
    def __init__(self, v: int) -> None:
        self.v = v
        self.ref = None

old: list = []
i: int = 0
while i < 20000:
    old.append(Box(i))
    i += 1

total: int = 0
j: int = 0
while j < 60000:
    n = Box(j)
    target = old[j % 20000]
    n.ref = target
    total += target.v
    j += 1
print(total)
print(len(old))

# --- expect ---
# 599970000
# 20000
