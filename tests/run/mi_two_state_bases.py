# 两个状态基类的多继承（0.510）：布局实体化 + 调用点 delta + vtable thunk 之后，
# 属性访问、直调、虚调用三条路径都按 CPython 的语义读字段。
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


cb: C = C()
print(cb.x, cb.get())   # 属性访问 + 直调（调用点 delta）
print(f(cb))            # 虚调用（vtable thunk）
print(cb.y)

# --- expect ---
# 1 1
# 1
# 2
