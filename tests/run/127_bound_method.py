# 127：方法值（bound method）：obj.m / C.m 取到的是绑定接收者的函数值
class Base:
    def __init__(self, x: int) -> None:
        self.x = x

    def shift(self, d: int) -> int:
        return self.x + d

    def bump(self, d: int = 1) -> int:
        return self.x + d

    @staticmethod
    def twice(v: int) -> int:
        return v * 2

    @classmethod
    def make(cls, x: int) -> "Base":
        return Base(x)


class Child(Base):
    def value(self) -> int:
        return self.x * 10

    def bump(self, d: int = 1) -> int:
        return self.x * 100 + d


def halve(f):
    def wrapper(self, d: int) -> int:
        return f(self, d) // 2
    return wrapper


class Doubler:
    def __init__(self, x: int) -> None:
        self.x = x

    @halve
    def shift(self, d: int) -> int:
        return (self.x + d) * 2


def apply(fn, v: int) -> int:
    return fn(v)


p = Base(10)
f = p.shift
print(f(5))

q = Base(100)
print(q.shift(5), f(1))

fs: list = [p.shift, q.shift]
print(fs[0](1), fs[1](1))
print(apply(q.shift, 2))

print(p.bump())
print(p.bump(3))
print(p.bump(d=4))

sm = Base.twice
print(sm(21))
print(p.twice(5))

cm = Base.make
print(cm(7).x)
print(p.make(8).x)

c = Child(3)
fc = c.bump
print(fc())
print(fc(2))
print(apply(c.bump, 5))
print(apply(p.shift, 5))

d = Doubler(3)
h = d.shift
print(h(4))
print(h(10))

opt: Base | None = None
if opt is not None:
    print(opt.shift(1))
else:
    print("none")

# --- expect ---
# 15
# 105 11
# 11 101
# 102
# 11
# 13
# 14
# 42
# 10
# 7
# 8
# 301
# 302
# 305
# 15
# 7
# 13
# none
