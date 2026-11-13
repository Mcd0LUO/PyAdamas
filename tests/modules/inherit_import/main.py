from lib import Base, Middle, make


class Sub(Middle):
    extra: int

    def __init__(self, tag: str, extra: int) -> None:
        super().__init__(tag)
        self.extra = extra

    def label(self) -> str:
        return "sub:" + self.tag

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

# --- expect ---
# x 5
# sub:x
# 111
# sub:x
# True True True
# m 11 base:m
# True False
# base:z True False
