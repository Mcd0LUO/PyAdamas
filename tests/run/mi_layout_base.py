# 多继承的字段前缀（0.504）：layout base = MRO 顺序里第一个**有字段**的基类。
# 它的字段在子类里保持原偏移，所以「mixin 在前、状态基类在后」不用改写法。
# 六种形态，输出必须与 CPython 逐字节一致。


# 1) 单继承
class Solo:
    x: int

    def __init__(self, x: int) -> None:
        self.x = x

    def get(self) -> int:
        return self.x


print(Solo(7).get())


# 2) C(A, B)：主基类带字段（老行为）
class Base:
    name: str

    def __init__(self, name: str) -> None:
        self.name = name

    def label(self) -> str:
        return "[" + self.name + "]"


class Plain:
    def tag(self) -> str:
        return "tag"


class Derived(Base, Plain):
    def __init__(self, name: str) -> None:
        self.name = name


d: Derived = Derived("d1")
print(d.label(), d.tag(), d.name)


# 3) C(B, A) 且 B **无字段**：layout base 是 A，A 的方法按自己布局读字段是对的
class Mixin:
    def log(self) -> str:
        return "mixin"


class State:
    sock: int

    def __init__(self) -> None:
        self.sock = 9

    def bind(self) -> int:
        return self.sock


class Combo(Mixin, State):
    def __init__(self) -> None:
        self.sock = 11


c: Combo = Combo()
print(c.sock, c.bind(), c.log())


# 4) 纯 mixin（无字段）在第二位
class Logger:
    def twice(self) -> str:
        return "l" + "l"


class Service(Base, Logger):
    def describe(self) -> str:
        return "svc:" + self.name


sv: Service = Service("s")
print(sv.describe(), sv.twice(), sv.label())


# 5) 菱形：共享祖先偏移不变
class Root:
    value: int

    def __init__(self, value: int) -> None:
        self.value = value

    def read(self) -> int:
        return self.value


class Left(Root):
    def left(self) -> int:
        return self.read() + 1


class Right(Root):
    def right(self) -> int:
        return self.read() + 2


class Diamond(Left, Right):
    pass


dia: Diamond = Diamond(10)
print(dia.read(), dia.left(), dia.right(), dia.value)


# 6) super() 链
class Sub(Base, Mixin):
    def __init__(self, name: str) -> None:
        super().__init__(name.upper())

    def describe(self) -> str:
        return "sub:" + self.name


sb: Sub = Sub("abc")
print(sb.name, sb.label(), sb.describe())

# --- expect ---
# 7
# [d1] tag d1
# 11 11 mixin
# svc:s ll [s]
# 10 11 12 10
# ABC [ABC] sub:ABC
