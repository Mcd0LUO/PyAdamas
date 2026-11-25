# B1-min（0.535）：绑定方法存进**局部变量**再调用（纯编译期表示）。
xs: list[float] = [1.0]
push = xs.append
push(2.0)
push(3.0)
print(xs)

s: str = "ab"
up = s.upper
print(up())


class C:
    def __init__(self, v: int) -> None:
        self.v = v
    def get(self) -> int:
        return self.v


c: C = C(7)
g = c.get
print(g())

# 重新赋值：按调用点当时的绑定走
d: C = C(9)
g = d.get
print(g())

# --- expect ---
# [1.0, 2.0, 3.0]
# AB
# 7
# 9
