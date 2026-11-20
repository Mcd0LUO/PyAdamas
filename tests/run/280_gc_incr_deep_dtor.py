# 增量标记正确性：深析构级联（30000 深链一次性 drop）+ churn 逼出 major。
class Link:
    def __init__(self, v: int) -> None:
        self.v = v
        self.next = None

head = None
i: int = 0
while i < 30000:
    n = Link(i)
    n.next = head
    head = n
    i += 1
s: int = 0
cur = head
while cur is not None:
    s += cur.v
    cur = cur.next
print(s)
head = None
j: int = 0
while j < 60000:
    junk = Link(j)
    j += 1
print(j)
# --- expect ---
# 449985000
# 60000
