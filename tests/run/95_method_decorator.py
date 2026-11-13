# 自定义方法装饰器：类创建时应用装饰器链（从下到上），实例调用通过
# 类属性里的包装函数间接调用；继承时沿 MRO 读到同一份类属性。
# 类接收者调用（C.m(obj, ...)）和 super() 目标也可以是被装饰的方法。
def traced(f):
    def wrapper(self, a: int, b: int) -> int:
        print("call", a, b)
        return f(self, a, b) + 100
    return wrapper


def bump(f):
    def wrapper(self, x: int) -> int:
        return f(self, x) + 1
    return wrapper


def double(f):
    def wrapper(self, x: int) -> int:
        return f(self, x) * 2
    return wrapper


class Calc:
    base: int = 10

    def __init__(self, v: int) -> None:
        self.v = v

    @traced
    def add(self, a: int, b: int) -> int:
        return a + b + self.v

    def plain(self, x: int) -> int:
        return x * 2


class Chain:
    @bump
    @double
    def val(self, x: int) -> int:
        return x


class Sub(Calc):
    pass


c = Calc(5)
print(c.add(1, 2))
print(c.plain(4))

ch = Chain()
print(ch.val(3))

s = Sub(7)
print(s.add(2, 3))

# 类接收者：显式传 self；继承来的装饰方法也能从子类类上调用。
print(Calc.add(c, 4, 5))
print(Sub.add(s, 1, 1))


class Base:
    @bump
    def val(self, x: int) -> int:
        return x * 3


class Kid(Base):
    def call(self, x: int) -> int:
        return super().val(x) + 1000


k = Kid()
print(k.call(2))

# --- expect ---
# call 1 2
# 108
# 8
# 7
# call 2 3
# 112
# call 4 5
# 114
# call 1 1
# 109
# 1007
