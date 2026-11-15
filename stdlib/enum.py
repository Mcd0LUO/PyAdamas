"""enum 模块的 pyadamas 子集（纯 pyadamas 源码，随编译器分发）。

支持 `Color.RED` 这种**类属性常量**用法：Enum / IntEnum / Flag / IntFlag /
StrEnum 都是空基类，枚举成员就是子类的类属性（int 或 str）。

语义边界（CPython 的元类语义做不到，见 docs/stdlib-gaps.md）：
- 成员没有 `.name` / `.value`；`Color(1)` 值查找、迭代、`__members__`、
  `auto()` 都不支持（`auto` 故意不导出，宁可在 import 时就报错）；
- `@unique` / `@verify` 只透传，不校验重复值；
- `IntEnum` 的 `==` 与整数比较是真比较（成员本身就是 int）。
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
    """重复值校验占位：直接返回类。"""
    return cls


def verify(cls):
    """构造后校验占位：直接返回类。"""
    return cls
