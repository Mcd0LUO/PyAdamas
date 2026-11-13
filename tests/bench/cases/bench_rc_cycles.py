"""RC/GC 基准：短命对象环，只能靠循环收集器回收。"""
class Node:
    def __init__(self, v: int) -> None:
        self.v = v
        self.next = None

i = 0
while i < 20000:
    a = Node(i)
    b = Node(i + 1)
    a.next = b
    b.next = a
    i += 1
print(i)
