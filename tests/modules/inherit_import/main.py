from lib import Base, Middle, make
from lib2 import Mixin


class Sub(Middle):
    extra: int

    def __init__(self, tag: str, extra: int) -> None:
        super().__init__(tag)
        self.extra = extra

    def label(self) -> str:
        return "sub:" + self.tag

    def value(self) -> int:
        return super().value() + 100


class Sub2(Middle):
    def value(self) -> int:
        return 7


class Multi(Middle, Mixin):
    def __init__(self, tag: str) -> None:
        super().__init__(tag)

    def value(self) -> int:
        return super().value() + 100


s = Sub("x", 5)
print(s.tag, s.extra)
print(s.label())
print(s.value())
b: Base = s
print(b.label())
print(isinstance(s, Sub), isinstance(s, Middle), isinstance(s, Base))

m = make("m")
print(m.tag, m.value(), m.label())
print(isinstance(m, Base), isinstance(m, Sub))

b2 = Base("z")
print(b2.label(), isinstance(b2, Base), isinstance(b2, Middle))

# 导入基类的字段默认值 + 跨模块 classmethod 重绑定
s2 = Sub2.make("q")
print(s2.tag, s2.flag)
print(s2.value())
print(Sub2.who())
print(Base.make("z").tag, isinstance(s2, Sub2), isinstance(s2, Base))

# 导入类作多继承附加基类：同名方法在不同模块的槽号不同
mi = Multi("m")
mi2: Middle = mi
mx: Mixin = mi
print(mi.tag, mi.flag)
print(mi.value(), mi2.value(), mx.value())
print(mx.mix())
print(isinstance(mi, Mixin), isinstance(mi, Middle))
print(Multi.who(), Base.who())

# --- expect ---
# x 5
# sub:x
# 111
# sub:x
# True True True
# m 11 base:m
# True False
# base:z True False
# q base
# 7
# Sub2
# z True True
# m base
# 111 111 111
# mixin
# True True
# Multi Base
