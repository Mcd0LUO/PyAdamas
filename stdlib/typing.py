"""typing 模块的 pyadamas 子集（纯 pyadamas 源码，随编译器分发）。

PyAdamas 的注解解析器**按名字**识别常用 typing 名字（List/Dict/Tuple/Set/
Optional/Union/Sequence/Iterable/Iterator/Mapping/Any 等），见
src/typecheck.cpp 的 parse_annotation：这些名字出现在函数/变量/字段标注里
时会被直接映射成内部类型，**不需要真的 import 本模块**。因此本模块的目标
只是让 `from typing import ...` 能编译通过，并提供少数值级 API。

语义边界（不做「静默算错」的假装支持）：
- 只用于注解：`Generic[T]` 泛型基类、`TypeVar` 约束、`NamedTuple`、
  `TypedDict`、`Protocol` 的结构校验都不参与检查；
- `cast()` 是恒等透传；`TYPE_CHECKING` 恒为 False；
- 值级下标的返回值无意义，仅用于 `cast(List[int], xs)` 这类表达式能编译。
"""


class _SpecialForm:
    """占位类型表达式：值级下标 `Form[...]` 一律返回 0。"""

    def __getitem__(self, item) -> int:
        return 0


# 注解解析器按名字特判的名字。值级下标（cast(List[int], xs)）需要可下标
# 对象，所以给实例；下标结果无意义，只保证表达式能编译。
List = _SpecialForm()
Dict = _SpecialForm()
Set = _SpecialForm()
FrozenSet = _SpecialForm()
Tuple = _SpecialForm()
Optional = _SpecialForm()
Union = _SpecialForm()
Iterable = _SpecialForm()
Iterator = _SpecialForm()
Sequence = _SpecialForm()
MutableSequence = _SpecialForm()
Mapping = _SpecialForm()
MutableMapping = _SpecialForm()
Collection = _SpecialForm()
Container = _SpecialForm()
Reversible = _SpecialForm()
Callable = _SpecialForm()
Type = _SpecialForm()
Generator = _SpecialForm()
AsyncGenerator = _SpecialForm()
AsyncIterable = _SpecialForm()
AsyncIterator = _SpecialForm()
Awaitable = _SpecialForm()
Coroutine = _SpecialForm()
Deque = _SpecialForm()
Counter = _SpecialForm()
DefaultDict = _SpecialForm()
OrderedDict = _SpecialForm()
ChainMap = _SpecialForm()
KeysView = _SpecialForm()
ItemsView = _SpecialForm()
ValuesView = _SpecialForm()
Pattern = _SpecialForm()
Match = _SpecialForm()
IO = _SpecialForm()
TextIO = _SpecialForm()
BinaryIO = _SpecialForm()
Text = _SpecialForm()
Any = _SpecialForm()


# 注解里通常**裸写**的名字（x: Final = 1）。注解解析器对它们没有特判，
# 会按作用域里的符号类型走：绑定成类/实例就会变成严格类型检查，绑定成
# 普通 int 值则退化成 unknown（宽松）。所以统一用 int 占位。
Final = 0
ClassVar = 0
Literal = 0
TypeAlias = 0
NoReturn = 0
Never = 0
Required = 0
NotRequired = 0
Unpack = 0
AnyStr = 0
Hashable = 0
Sized = 0
SupportsInt = 0
ParamSpec = 0
TypeVarTuple = 0
Concatenate = 0
NamedTuple = 0
TypedDict = 0
Protocol = 0
Generic = 0

TYPE_CHECKING = False


def cast(ty, v):
    """恒等透传；类型只用于静态检查。"""
    return v


def TypeVar(name: str) -> int:
    """类型变量占位：返回 int 值，注解里会退化成 unknown（宽松）。"""
    return 0


def NewType(name: str, tp):
    """返回一个可调用对象：值级上等价于它的基础类型。"""
    return int


def overload(f):
    return f


def final(f):
    return f


def override(f):
    return f


def runtime_checkable(f):
    return f


def no_type_check(f):
    return f


def get_type_hints(obj) -> dict:
    return {}


def get_args(t) -> list:
    return []


def get_origin(t) -> int:
    return 0


def assert_type(v, t):
    return v


def assert_never(x) -> int:
    return 0


def reveal_type(x) -> None:
    print(x)
