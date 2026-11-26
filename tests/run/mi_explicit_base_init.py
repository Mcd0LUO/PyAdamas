# BUG-1：多继承下显式调两个基类的 __init__，字段块偏移必须平移。
# 修复前：C 的布局是 [A 的前缀][B 的块][C 自己的字段]，但 B.__init__(self)
# 按 B 自己的布局写 self.b（偏移 0），覆盖了 A 的 a —— 输出 2 0 3。
class A:
    a: int
    def __init__(self) -> None:
        self.a = 1
    def ga(self) -> int:
        return self.a * 10

class B:
    b: int
    def __init__(self) -> None:
        self.b = 2
    def gb(self) -> int:
        return self.b * 10

class C(A, B):
    c: int
    def __init__(self) -> None:
        A.__init__(self)
        B.__init__(self)
        self.c = 3

x = C()
print(x.a, x.b, x.c)
print(x.ga(), x.gb())

# 子类的布局前缀不变，同一套偏移继续成立
class D(C):
    d: int
    def __init__(self) -> None:
        C.__init__(self)
        self.d = 4

y = D()
print(y.a, y.b, y.c, y.d)
print(y.ga(), y.gb())

# 显式调用写在自由函数里同样要平移
def rebuild(o: C) -> None:
    A.__init__(o)
    B.__init__(o)

z = C()
rebuild(z)
print(z.a, z.b, z.c)

# 单继承不受影响
class S(B):
    s: int
    def __init__(self) -> None:
        B.__init__(self)
        self.s = 7

u = S()
print(u.b, u.s)

# --- expect ---
# 1 2 3
# 10 20
# 1 2 3 4
# 10 20
# 1 2 3
# 2 7
