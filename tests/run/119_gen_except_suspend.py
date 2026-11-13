# 挂起点在 except 体内：异常上下文跟着 resume 走，调用方的 except 状态
# 在生成器挂起期间保持不变；close 从 except 体退出也会跑完清理。
import asyncio


def gen():
    try:
        raise ValueError("boom")
    except ValueError as e:
        print("caught", e)
        x = yield 1
        print("after", x)
    yield 2


it = gen()
v = next(it)
print("got", v)
v = it.send(5)
print("got", v)
try:
    it.send(0)
except StopIteration:
    print("stop")


def g2():
    try:
        raise ValueError("boom2")
    except ValueError:
        yield 1
        raise
    yield 2


i2 = g2()
v = next(i2)
print("got2", v)
try:
    i2.send(0)
except ValueError as e:
    print("reraised", e)


def inner():
    try:
        raise KeyError("inner")
    except KeyError:
        yield "y"


def caller():
    try:
        raise ValueError("outer")
    except ValueError:
        gi = inner()
        print("inner", next(gi))
        try:
            raise
        except ValueError as e:
            print("outer ok", e)


caller()


def g3():
    try:
        raise RuntimeError("r")
    except RuntimeError:
        try:
            yield 1
        finally:
            print("cleanup3")


i3 = g3()
print("g3", next(i3))
i3.close()
print("closed")


def looper():
    for i in range(3):
        try:
            if i == 1:
                raise RuntimeError("r")
        except RuntimeError:
            yield i * 10
        else:
            yield i


out = []
for v in looper():
    out.append(v)
print(out)


async def coro_retry() -> str:
    try:
        raise ValueError("first")
    except ValueError:
        await asyncio.sleep(0)
        return "ok"


async def main() -> None:
    r = await coro_retry()
    print("coro", r)
    t = asyncio.create_task(coro_retry())
    await asyncio.sleep(0)
    t.cancel()
    try:
        await t
    except CancelledError:
        print("cancelled in except")


asyncio.run(main())

# --- expect ---
# caught boom
# got 1
# after 5
# got 2
# stop
# got2 1
# reraised boom2
# inner y
# outer ok outer
# g3 1
# cleanup3
# closed
# [0, 10, 2]
# coro ok
# cancelled in except
