"""enum 模块的 pyadamas 子集（纯 pyadamas 源码，随编译器分发）。

编译期支持 Enum 子类的真正枚举语义（class 语句被编译器拦截，成员变成成员对象）：
- `class Color(Enum): RED = 1`：成员对象有 `.name` / `.value`；
- `Color.RED`、`Color(1)` 按值反查、`for c in Color` / `list(Color)`、
  `==` / `is`（成员是单例，`Color.RED is Color(1)` 为 True）；
- `IntEnum` / `IntFlag` / `StrEnum` 仍是 int/str 子类（成员就是 int，与 CPython 一致）；
- `Flag` 保持旧的类属性常量语义。

限制（CPython 的元类语义里暂不支持的部分）：别名（重复值）、`auto()`、
混合 int/str 成员值、`Color.__members__`、自定义 `__init__`/`__new__`、
按名字拼接的枚举（functional API）。
"""


class Enum:
    pass


class IntEnum(int):
    pass


class Flag:
    pass


class IntFlag(int, Flag):
    pass


class StrEnum(str):
    pass


class EnumMeta:
    pass


class EnumType:
    pass


def unique(cls):
    """重复值校验占位：类体成员在编译期已校验，直接返回类。"""
    return cls


def verify(cls):
    """构造后校验占位：直接返回类。"""
    return cls


# ---- 0.570：CPython 3.11+ 的 global_* 装饰器（子集） ----
# 成员提升到模块作用域由**编译器**完成（enum 成员的名称集合是编译期已知的），
# 这里只需要让装饰器可解析、返回原类。
def global_enum(cls):
    return cls


def global_enum_repr(cls):
    return cls


def global_str(cls):
    return cls


def global_flag_repr(cls):
    return cls


def global_flag_str(cls):
    return cls
