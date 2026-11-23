"""abc 模块的 pyadamas 子集（纯 pyadamas 源码，随编译器分发）。

`class Shape(ABC)` + `@abstractmethod` 声明式用法可用，而且是**真的**：
- `@abstractmethod` 标记方法（编译期事实，运行期装饰器仍是恒等函数）；
- 含未实现抽象方法的类**不能实例化**：编译期报
  "cannot instantiate abstract class 'A' (abstract method 'f' is not implemented)"；
- 子类实现了全部抽象方法后即可实例化（抽象方法可被普通方法覆盖）。

限制：`ABCMeta` 不能当 metaclass 用（metaclass= 仍未建模）；
`__isabstractmethod__` 运行期属性、`abc.register()` 虚拟子类不支持。
"""


class ABC:
    pass


class ABCMeta:
    pass


def abstractmethod(f):
    """抽象方法标记：编译期由 typecheck 识别并记录，运行期原样返回函数。"""
    return f


def abstractclassmethod(f):
    return f


def abstractstaticmethod(f):
    return f
