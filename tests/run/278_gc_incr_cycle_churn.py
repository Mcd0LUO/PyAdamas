# 增量标记正确性：标记期 cycle churn（Node 环 + dict 自环），大分配量逼出 major。
class Node:
    def __init__(self, v: int) -> None:
        self.v = v
        self.next = None

total: int = 0
i: int = 0
while i < 60000:
    a = Node(i)
    b = Node(i + 1)
    a.next = b
    b.next = a
    total += a.v + b.v
    i += 1
print(total)

j: int = 0
acc: int = 0
while j < 30000:
    d = {}
    d["self"] = d
    d["val"] = j
    acc += d["val"]
    j += 1
print(acc)
# --- expect ---
# 3600000000
# 449985000
