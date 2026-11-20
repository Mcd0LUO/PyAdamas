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
- suppress() / closing() 不在本模块：suppress 要把异常类作为普通值存起来
  再和 with 协议的异常类比较，但异常类过 unknown 参数只剩 0（同一性丢失）；
  closing 要在任意对象上调用 `.close()`，pa 对 unknown 值不支持方法调用。
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
