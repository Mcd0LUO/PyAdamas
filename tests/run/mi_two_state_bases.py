# 两个状态基类的多继承（0.510）：属性访问 / 直调 / 虚调用 / super() 四条路径。
class A:
    x: int
    def get(self) -> int:
        return self.x


class B:
    y: int


class C(B, A):
    def __init__(self) -> None:
        self.y = 2
        self.x = 1


def f(a: A) -> int:
    return a.get()


class D(C):
    def get2(self) -> int:
        return super().get()


cb: C = C()
print(cb.x, cb.get())   # 属性访问 + 直调（调用点 delta）
print(f(cb))            # 虚调用（vtable thunk）
print(cb.y)
d: D = D()
print(d.get2())         # super() 进入非 layout 基类的方法（调用点 delta + 静态直调）
print(d.get())          # 继承来的实现，仍走 thunk

# --- expect ---
# 1 1
# 1
# 2
# 1
# 1
