"""contextlib —— with 语句的工具（纯 pyadamas 源码，stdlib3）。

覆盖 nullcontext / @contextmanager 与 _GeneratorContextManager。

语义边界（轻量标尺，见 docs/stdlib-batch3.md）：
- **@contextmanager 无法把 with 体的异常注回生成器**：pa 的异常类/实例
  跨 unknown 形参会退化成裸 8 字节（生成器 throw() 只认静态异常名/实例），
  所以被装饰的生成器里 `try: yield ... except E:` 捕获不到 with 体抛出的
  异常。实现口径：异常路径对生成器 `close()`，`finally`/`with` 清理照常
  执行，异常本身原样向外抛（**从不吞异常**，也不用 throw 注回）。
- 包装函数只转发 0~4 个**位置**参数：把 `*args` 转发给 unknown 被装饰者
  在 pa 里只能走定长实参，关键字参数会抛 TypeError。
- suppress() / closing()（P4，PYADAMAS_LANG_GAPS=1）：异常类当值后
  （ClassObj 单例 + CLASS tag=11），suppress 把类值存进 unknown 元组，
  __exit__ 用内部 intrinsic __pa_exc_is_a(et, E) 按名字判父子（with 协议
  的 et 是类型名串）；closing 用 __pa_close(thing) 做零参动态 .close()
  （file 直调 / 类实例按运行期方法名→槽登记表沿 MRO 分派）。
  边界：closing.__enter__ 返回 unknown，`as g` 拿到的值不支持方法调用，
  请用原引用；门控关时异常类值退化成 0，suppress 不可用（运行期
  __pa_exc_is_a 报 TypeError）。
"""


class nullcontext:
    """空的 with 上下文，__enter__ 原样返回 enter_result。"""

    enter_result: unknown

    def __init__(self, enter_result: unknown = None) -> None:
        self.enter_result = enter_result

    def __enter__(self) -> unknown:
        return self.enter_result

    def __exit__(self, et, ev, tb) -> bool:
        return False


class _GeneratorContextManager:
    """@contextmanager 包装出来的上下文对象。"""

    gen: Generator[unknown]

    def __init__(self, gen: Generator[unknown]) -> None:
        self.gen = gen

    def __enter__(self) -> unknown:
        g: Generator[unknown] = self.gen
        return next(g)

    def __exit__(self, et, ev, tb) -> bool:
        g: Generator[unknown] = self.gen
        if et is None:
            # 用 for 循环推进生成器（而不是 next()+except StopIteration）：
            # pa 的异常值对象在被 __exit__ 的 handler 接收后会留到进程退出，
            # ASan detect_leaks=1 报 176 字节泄漏（见 docs/stdlib-batch3.md
            # 已知问题）。for 循环的正常结束路径不建异常值，干净。
            extra = False
            for _ in g:
                extra = True
                break
            if not extra:
                return False
            g.close()
            raise RuntimeError("generator didn't stop")
        # 异常注回做不到（见模块文档）：用 close() 跑生成器里的
        # finally/with 清理，异常本身原样向外抛（不吞）。
        g.close()
        return False


def _make_cm(func: unknown, args: unknown) -> _GeneratorContextManager:
    f = func
    n = len(args)
    if n == 0:
        g = f()
    elif n == 1:
        g = f(args[0])
    elif n == 2:
        g = f(args[0], args[1])
    elif n == 3:
        g = f(args[0], args[1], args[2])
    elif n == 4:
        g = f(args[0], args[1], args[2], args[3])
    else:
        raise TypeError("contextmanager supports at most 4 positional arguments")
    return _GeneratorContextManager(g)


def contextmanager(func: unknown):
    """把「yield 一次的生成器函数」装饰成 with 上下文工厂。"""

    def helper(*args) -> _GeneratorContextManager:
        return _make_cm(func, args)

    return helper


ContextManager = _GeneratorContextManager


class suppress:
    """with 块里抑制指定异常（CPython contextlib.suppress 口径）。

    异常类以运行期类对象值存进 unknown 元组；__exit__ 收到的 et 是
    with 协议给的类型名串，__pa_exc_is_a 按 exc_is_a 父子链匹配。
    不匹配的异常原样外抛；正常路径（et is None）不抑制。
    """

    _exceptions: unknown

    def __init__(self, *exceptions) -> None:
        self._exceptions = exceptions

    def __enter__(self) -> None:
        return None

    def __exit__(self, et, ev, tb) -> bool:
        if et is None:
            return False
        for E in self._exceptions:
            if __pa_exc_is_a(et, E):
                return True
        return False


class closing:
    """with 块结束时保证 thing.close()（CPython contextlib.closing 口径）。

    thing 过 unknown：file 对象由运行期直调 pa_file_close；类实例按
    方法名→槽登记表沿 MRO 找 close 实现。不抑制异常（__exit__ 恒 False）。
    """

    thing: unknown

    def __init__(self, thing) -> None:
        self.thing = thing

    def __enter__(self) -> unknown:
        return self.thing

    def __exit__(self, et, ev, tb) -> bool:
        __pa_close(self.thing)
        return False
