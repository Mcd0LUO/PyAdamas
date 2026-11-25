# 0.542：buffer 作类字段（S2）：标注 `buf: buffer` 必须解析成 buffer 类型，
# 否则字段读回是 unknown，下标写会走列表路径 → 段错误。
class W:
    buf: buffer
    def __init__(self, n: int) -> None:
        self.buf = buffer(n)
    def set(self, i: int, v: float) -> None:
        self.buf[i] = v
    def get(self, i: int) -> float:
        return self.buf[i]


w: W = W(10)
w.buf[2] = 1.5
print(len(w.buf), w.buf[2])

# 跨语句 + 跨函数
w.set(3, 2.5)
print(w.get(3))

# 跨对象
v: W = W(4)
v.buf[0] = 9.5
print(len(v.buf), v.buf[0], w.buf[2])

# 传出去再读
def total_second(x: W) -> float:
    return x.buf[2]


print(total_second(w))

# --- expect ---
# 10 1.5
# 2.5
# 4 9.5 1.5
# 1.5
