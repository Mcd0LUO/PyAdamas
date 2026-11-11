# 39_hasattr —— hasattr(实例, "字段")：编译期折叠
N = 1000000
class P:
    x: int

    def __init__(self) -> None:
        self.x = 0


p = P()
total = 0
for i in range(N):
    total += 1 if hasattr(p, "x") else 0
print(total)
