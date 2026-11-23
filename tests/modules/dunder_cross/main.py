# 跨模块 dunder：导入基类的 __eq__/__lt__ + 子类重写 __eq__，
# unknown 域（混合容器取出的实例）按 vtable dunder 槽表分派。
import base


class Child(base.Base):
    def __init__(self, v: int) -> None:
        super().__init__(v)

    def __eq__(self, other: "Child") -> bool:
        return self.v * 10 == other.v * 10


def eq(a, b) -> bool:
    return a == b


c1 = Child(2)
c2 = Child(2)
print("typed_child", c1 == c2)
b1 = base.make(3)
xs = [b1, 1]
a = xs[0]
print("unk_base", eq(a, base.make(3)), eq(a, base.make(4)))
cs = [Child(1), 1]
c = cs[0]
print("unk_child", eq(c, Child(1)), eq(c, Child(2)))
ys = [base.make(3), base.make(1), base.make(2)]
print("sorted_base", [p.v for p in sorted(ys)])

# --- expect ---
# typed_child True
# unk_base True False
# unk_child True False
# sorted_base [1, 2, 3]
