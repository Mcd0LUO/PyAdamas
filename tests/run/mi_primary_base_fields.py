# 多继承字段布局（3.9）：非主基类的方法碰字段会被拒绝；下面全是**允许**的形态，
# 输出必须与 CPython 逐字节一致。拒绝的那条在
# tests/strict/mi_non_primary_base_fields.py。


# 1) 单继承：完全不受影响
class Solo:
    x: int

    def __init__(self, x: int) -> None:
        self.x = x

    def get(self) -> int:
        return self.x


s: Solo = Solo(7)
print(s.get(), s.x)


# 2) 多继承、主基类（第一个）带字段：允许（布局前缀不变）
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


# 4) 纯 mixin：非主基类没有字段，方法也不碰 self.<字段> —— 必须继续允许
class Logger:
    def log(self, msg: str) -> str:
        return "log:" + msg

    def twice(self) -> str:
        return self.log("a") + self.log("b")


class Service(Base, Logger):  # 状态在主基类，Logger 是纯 mixin（无字段）
    def __init__(self, name: str) -> None:
        self.name = name


svc: Service = Service("svc")
print(svc.twice(), svc.label(), svc.name)


# 5) 菱形：共享祖先的偏移在子类里不变 → 允许
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


# 6) super() 链：主基类前缀 + 附加 mixin，协作调用与 CPython 一致
class Mixin:
    def describe(self) -> str:
        return "mixin"


class Sub(Base, Mixin):
    def __init__(self, name: str) -> None:
        super().__init__(name.upper())

    def describe(self) -> str:
        return "sub:" + self.name


sb: Sub = Sub("abc")
print(sb.name, sb.label(), sb.describe())

# --- expect ---
# 7 7
# [d1] tag d1
# log:alog:b [svc] svc
# 10 11 12 10
# ABC [ABC] sub:ABC
