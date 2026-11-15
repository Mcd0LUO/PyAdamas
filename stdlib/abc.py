"""abc 模块的 pyadamas 子集（纯 pyadamas 源码，随编译器分发）。

`class Shape(ABC)` + `@abstractmethod` 这种声明式用法可用；抽象方法不会
阻止实例化（没有元类），`ABCMeta` 也不能当 metaclass 用。
"""


class ABC:
    pass


class ABCMeta:
    pass


def abstractmethod(f):
    """抽象方法占位：原样返回函数。"""
    return f


def abstractclassmethod(f):
    return f


def abstractstaticmethod(f):
    return f
