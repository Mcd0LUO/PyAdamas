# A1 静默错值回归：类实例下标返回 float，直接用（print/str/unknown 参数/容器）
# 过去 print(b[1]) 会打出 double 的整数位模式。
class Box:
    data: list[float]
    def __init__(self, d: list[float]) -> None:
        self.data = d
    def __getitem__(self, i: int) -> float:
        return self.data[i]


b: Box = Box([1.5, 2.5, 3.5])
print(b[1])
v: float = b[1]
print(v)
print(str(b[1]))
print(repr(b[1]))
xs: list[float] = [b[0], b[2]]
print(xs)
total: float = 0.0
total = total + b[2]
print(total)

# --- expect ---
# 2.5
# 2.5
# 2.5
# 2.5
# [1.5, 3.5]
# 3.5
