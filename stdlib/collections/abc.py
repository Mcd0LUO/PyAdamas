"""collections.abc 的 pyadamas 子集（纯 pyadamas 源码，随编译器分发）。

只提供可 import / 可当注解用的空基类。**没有运行期协议检查**：
`isinstance([1], Iterable)` 会返回 False，别把它当语义正确的结果；
注解位置上的 Iterable/Sequence/Mapping 等名字由注解解析器特判成宽松类型。
"""


class Container:
    pass


class Hashable:
    pass


class Sized:
    pass


class Iterable:
    pass


class Iterator:
    pass


class Reversible:
    pass


class Generator:
    pass


class Collection:
    pass


class Sequence:
    pass


class MutableSequence:
    pass


class ByteString:
    pass


class Set:
    pass


class MutableSet:
    pass


class Mapping:
    pass


class MutableMapping:
    pass


class MappingView:
    pass


class KeysView:
    pass


class ItemsView:
    pass


class ValuesView:
    pass


class Awaitable:
    pass


class Coroutine:
    pass


class AsyncIterable:
    pass


class AsyncIterator:
    pass


class AsyncGenerator:
    pass


class Callable:
    pass
