# 实例分配循环（P2-C）：创建 / 字段读写 / 异常路径。与 CPython 逐字节一致。
class V:
    def __init__(self, x: float) -> None:
        self.x = x
    def get(self) -> float:
        return self.x


i: int = 0
acc: float = 0.0
while i < 1000:
    v: V = V(1.5)
    acc = acc + v.get()
    i += 1
print(int(acc))

v2: V = V(2.5)
v2.x = 3.5
print(v2.get())

try:
    v3: V = V(1.0)
    raise ValueError('boom')
except ValueError as e:
    print('caught', e)

# --- expect ---
# 1500
# 3.5
# caught boom
