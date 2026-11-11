# 38_setattr —— setattr(实例, "字段", 值)
N = 1000000
class P:
    x: int

    def __init__(self) -> None:
        self.x = 0


p = P()
total = 0
for i in range(N):
    setattr(p, "x", i)
    total += p.x
print(total)
