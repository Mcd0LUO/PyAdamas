# expect: base 'A' of 'C' is not the primary base and its methods access instance fields; put A first or make it a stateless mixin (see docs/mi-layout.md)
# 3.9：非主基类 A 带字段、且 A 的方法体读 self.x —— 在 C 里 x 被 B 顶到后面，
# 方法体仍按 A 的布局算偏移，会静默读到 y（CB 上是 10 20，CPython 是 10 10）。
# 编译期直接拒绝，不拖到运行期。


class A:
    x: int

    def get(self) -> int:
        return self.x


class B:
    y: int


class C(B, A):
    def __init__(self) -> None:
        self.y = 20
        self.x = 10


cb: C = C()
print(cb.x, cb.get())
