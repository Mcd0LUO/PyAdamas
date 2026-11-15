# PEP 479：生成器 / 异步生成器 / 协程帧里逃出的 StopIteration 一律转
# RuntimeError（CPython 3.7+），for/async for 不能把它当正常结束静默收尾；
# 生成器内部正常捕获 StopIteration 的语义不受影响（内部 try 先接住）。
import asyncio


def g_loop():
    yield 1
    raise StopIteration("x")


# 1) for 循环：逃出的 StopIteration 必须变成 RuntimeError
try:
    for v in g_loop():
        print("loop", v)
except StopIteration:
    print("BAD loop: leaked StopIteration")
except RuntimeError as e:
    print("loop RuntimeError:", e)

# 2) next() 直接驱动
it = g_loop()
print("next1", next(it))
try:
    next(it)
except StopIteration:
    print("BAD next: leaked StopIteration")
except RuntimeError as e:
    print("next RuntimeError:", e)

# 3) throw() 注入的 StopIteration 没被生成器接住时同样转换
def g_throw():
    yield 1
    yield 2


t = g_throw()
print("throw1", next(t))
try:
    t.throw(StopIteration("t"))
except StopIteration:
    print("BAD throw: leaked StopIteration")
except RuntimeError as e:
    print("throw RuntimeError:", e)

# 4) close() 重放 finally 时抛出的 StopIteration
def g_close():
    try:
        yield 1
    finally:
        raise StopIteration("f")


c = g_close()
print("close1", next(c))
try:
    c.close()
except StopIteration:
    print("BAD close: leaked StopIteration")
except RuntimeError as e:
    print("close RuntimeError:", e)

# 5) StopIteration 子类同样转换
class MyStop(StopIteration):
    pass


def g_sub():
    yield 1
    raise MyStop("sub")


try:
    for v in g_sub():
        print("sub", v)
except StopIteration:
    print("BAD sub: leaked StopIteration")
except RuntimeError as e:
    print("sub RuntimeError:", e)

# 6) 生成器内部正常捕获：不能转换、不能影响 for 收尾
def g_inner():
    yield 1
    try:
        raise StopIteration("inner")
    except StopIteration:
        yield "caught"
    yield 3


print("inner", list(g_inner()))

# 7) 内部每轮捕获后正常 yield，for 正常收尾
def g_inner2():
    for i in range(3):
        try:
            raise StopIteration("boom")
        except StopIteration:
            yield i


print("inner2", list(g_inner2()))


async def ag_loop():
    yield 1
    raise StopIteration("a")


async def ag_throw():
    yield 1
    yield 2


async def ag_close():
    try:
        yield 1
    finally:
        raise StopIteration("f")


async def ag_inner():
    yield 1
    try:
        raise StopIteration("inner")
    except StopIteration:
        yield "caught"
    yield 3


async def c_stop():
    raise StopIteration("c")


async def main() -> None:
    # 8) async for
    try:
        async for v in ag_loop():
            print("asyncfor", v)
    except StopIteration:
        print("BAD asyncfor: leaked StopIteration")
    except RuntimeError as e:
        print("asyncfor RuntimeError:", e)

    # 9) __anext__
    a = ag_loop()
    print("anext1", await a.__anext__())
    try:
        await a.__anext__()
    except StopIteration:
        print("BAD anext: leaked StopIteration")
    except RuntimeError as e:
        print("anext RuntimeError:", e)

    # 10) athrow()
    b = ag_throw()
    print("athrow1", await b.asend(None))
    try:
        await b.athrow(StopIteration("t"))
    except StopIteration:
        print("BAD athrow: leaked StopIteration")
    except RuntimeError as e:
        print("athrow RuntimeError:", e)

    # 11) aclose()
    d = ag_close()
    print("aclose1", await d.__anext__())
    try:
        await d.aclose()
    except StopIteration:
        print("BAD aclose: leaked StopIteration")
    except RuntimeError as e:
        print("aclose RuntimeError:", e)

    # 12) 异步生成器内部正常捕获
    vals = []
    async for v in ag_inner():
        vals.append(v)
    print("ag inner", vals)

    # 13) 协程里逃出的 StopIteration（CPython 同样转 RuntimeError）
    try:
        await c_stop()
    except StopIteration:
        print("BAD coro: leaked StopIteration")
    except RuntimeError as e:
        print("coro RuntimeError:", e)


asyncio.run(main())

# 14) yield from：子生成器逃出的 StopIteration 同样先转 RuntimeError 再向外传
def g_sub2():
    yield 1
    raise StopIteration("s")


def g_outer():
    yield from g_sub2()
    yield 99


try:
    for v in g_outer():
        print("yf", v)
except StopIteration:
    print("BAD yf: leaked StopIteration")
except RuntimeError as e:
    print("yf RuntimeError:", e)

# --- expect ---
# loop 1
# loop RuntimeError: generator raised StopIteration
# next1 1
# next RuntimeError: generator raised StopIteration
# throw1 1
# throw RuntimeError: generator raised StopIteration
# close1 1
# close RuntimeError: generator raised StopIteration
# sub 1
# sub RuntimeError: generator raised StopIteration
# inner [1, caught, 3]
# inner2 [0, 1, 2]
# asyncfor 1
# asyncfor RuntimeError: async generator raised StopIteration
# anext1 1
# anext RuntimeError: async generator raised StopIteration
# athrow1 1
# athrow RuntimeError: async generator raised StopIteration
# aclose1 1
# aclose RuntimeError: async generator raised StopIteration
# ag inner [1, caught, 3]
# coro RuntimeError: coroutine raised StopIteration
# yf 1
# yf RuntimeError: generator raised StopIteration
