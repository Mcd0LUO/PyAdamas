# finally 体内的挂起：yield / yield from / await 都可以出现在 finally 块里，
# 异常路径下 finalbody 结束后继续重抛原异常；close 重放到挂起点时按 CPython
# 报 RuntimeError（generator ignored GeneratorExit）。
import asyncio


class Ctx:
    def __enter__(self) -> int:
        print("enter")
        return 0

    def __exit__(self, t, v, tb) -> bool:
        print("exit")
        return False


def normal():
    try:
        print("try")
        yield 1
    finally:
        print("fin1")
        yield 2
        print("fin2")


it = normal()
print(next(it))
print(next(it))
try:
    next(it)
except StopIteration:
    print("stop1")


def rethrow():
    try:
        raise ValueError("boom")
    finally:
        print("fin")
        yield 2
        print("fin2")


it = rethrow()
print(next(it))
try:
    next(it)
except ValueError as e:
    print("caught", e)


def closing():
    try:
        yield 1
    finally:
        print("cleanup")
        yield 2
        print("more")


it = closing()
print(next(it))
try:
    it.close()
except RuntimeError as e:
    print("close:", e)


def breaking():
    for i in range(2):
        try:
            break
        finally:
            print("f")
            yield i
    print("after")


it = breaking()
print(next(it))
try:
    next(it)
except StopIteration:
    print("stop4")


def nested():
    try:
        try:
            raise ValueError("v")
        finally:
            yield 1
            print("inner rest")
    finally:
        print("outer cleanup")


it = nested()
print(next(it))
it.close()
print("closed5")


def sub():
    yield 10
    yield 20


def delegating():
    try:
        yield 1
    finally:
        yield from sub()


it = delegating()
print(next(it))
print(next(it))
print(next(it))
try:
    next(it)
except StopIteration:
    print("stop6")


def looping():
    try:
        yield 0
    finally:
        i = 0
        while i < 2:
            i += 1
            yield i


it = looping()
print(next(it))
print(next(it))
print(next(it))
try:
    next(it)
except StopIteration:
    print("stop7")


def with_inside():
    try:
        yield 0
    finally:
        with Ctx():
            yield 1
        print("after with")


it = with_inside()
print(next(it))
print(next(it))
try:
    next(it)
except StopIteration:
    print("stop8")


def sending():
    try:
        yield 1
    finally:
        x = yield 2
        print("sent", x + 1)


it = sending()
print(next(it))
print(it.send(9))
try:
    it.send(10)
except StopIteration:
    print("stop9")


def returning():
    try:
        return 7
    finally:
        yield 1
        print("fin done")


it = returning()
print(next(it))
try:
    next(it)
except StopIteration:
    print("stop10")


async def work(x: int) -> int:
    return x * 2


async def async_normal() -> int:
    try:
        print("astart")
        return 1
    finally:
        print("afin")
        v: int = await work(3)
        print("got", v)


async def async_bad() -> None:
    try:
        raise ValueError("aboom")
    finally:
        print("abadfin")
        v: int = await work(2)
        print("got", v)


async def amain() -> None:
    r = await async_normal()
    print("r", r)
    try:
        await async_bad()
    except ValueError as e:
        print("caught", e)


asyncio.run(amain())


# --- expect ---
# try
# 1
# fin1
# 2
# fin2
# stop1
# fin
# 2
# fin2
# caught boom
# 1
# cleanup
# close: generator ignored GeneratorExit
# f
# 0
# after
# stop4
# 1
# outer cleanup
# closed5
# 1
# 10
# 20
# stop6
# 0
# 1
# 2
# stop7
# 0
# enter
# 1
# exit
# after with
# stop8
# 1
# 2
# sent 11
# stop9
# 1
# fin done
# stop10
# astart
# afin
# got 6
# r 1
# abadfin
# got 4
# caught aboom
