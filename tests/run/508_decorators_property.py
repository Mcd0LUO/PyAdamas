# 装饰器与属性协议回归（0.441）：函数/方法/类装饰器、property/setter/deleter、
# staticmethod/classmethod、方法值绑定、装饰 __call__/__init__/__len__。
calls: list = []


def trace(fn):
    def wrapper(x: int) -> int:
        calls.append(x)
        return fn(x) * 10
    return wrapper


def dbl(x: int) -> int:
    return x * 2


f = trace(dbl)
print("wrap", f(3), calls)
@trace
def g(x: int) -> int:
    return x + 1


print("deco", g(2), calls)
def add_one(fn):
    def w(x: int) -> int:
        return fn(x) + 1
    return w


def times_two(fn):
    def w(x: int) -> int:
        return fn(x) * 2
    return w


@add_one
@times_two
def h(x: int) -> int:
    return x


print("stack", h(5))
def make_adder(n: int):
    def dec(fn):
        def w(x: int) -> int:
            return fn(x) + n
        return w
    return dec


@make_adder(100)
def k(x: int) -> int:
    return x


print("arg_deco", k(1))
def deco_m(fn):
    def w(self, x: int) -> int:
        return fn(self, x) + 1
    return w


class M:
    @deco_m
    def m(self, x: int) -> int:
        return x * 2


print("method_deco", M().m(3), M.m(M(), 10))
class Store:
    val: int

    def __init__(self) -> None:
        self.val = 0

    @property
    def x(self) -> int:
        return self.val

    @x.setter
    def x(self, v: int) -> None:
        self.val = v * 2


s = Store()
s.x = 21
print("property", s.x, s.val)
class Temp:
    _c: int

    def __init__(self, c: int) -> None:
        self._c = c

    @property
    def f(self) -> int:
        return self._c * 9 // 5 + 32


print("getter", Temp(100).f)
class Holder:
    _v: int

    def __init__(self) -> None:
        self._v = 4

    @property
    def v(self) -> int:
        return self._v


class H2(Holder):
    @property
    def v(self) -> int:
        return 99


print("prop_inherit", Holder().v, H2().v)
class MathTool:
    label = "tool"
    value: int

    def __init__(self) -> None:
        self.value = 7

    @staticmethod
    def add(a: int, b: int) -> int:
        return a + b

    @classmethod
    def describe(cls) -> str:
        return "label:" + cls.label

    @classmethod
    def make(cls):
        return cls()


print("static", MathTool.add(1, 2), MathTool().add(3, 4))
print("classmeth", MathTool.describe(), MathTool().describe(), MathTool.make().value)
adder = MathTool.add
print("static_value", adder(7, 8))
class Base:
    def greet(self) -> str:
        return "hi"


class Child(Base):
    def greet(self) -> str:
        return "child-hi"


b = Base()
print("bind", b.greet(), Child().greet(), b.greet())
def apply(f) -> str:
    return f()


print("bind_arg", apply(Child().greet))
def deco_call(fn):
    def w(self, x: int) -> int:
        return fn(self, x) * 2
    return w


class Made:
    @deco_call
    def __call__(self, x: int) -> int:
        return x + 1


print("deco_call", Made()(3))
def deco_init(init):
    def w(self, v: int) -> None:
        init(self, v * 2)
    return w


class A:
    x: int

    @deco_init
    def __init__(self, v: int) -> None:
        self.x = v


print("deco_init", A(5).x)
def deco_len(ln):
    def w(self) -> int:
        return ln(self) + 100
    return w


class L:
    @deco_len
    def __len__(self) -> int:
        return 3


print("deco_len", len(L()))

# --- expect ---
# wrap 60 [3]
# deco 30 [3, 2]
# stack 11
# arg_deco 101
# method_deco 7 21
# property 42 42
# getter 212
# prop_inherit 4 99
# static 3 7
# classmeth label:tool label:tool 7
# static_value 15
# bind hi child-hi hi
# bind_arg child-hi
# deco_call 8
# deco_init 10
# deco_len 103
