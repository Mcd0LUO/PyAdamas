# 异步生成器里的 await：结果值必须送回帧槽（而不是被 driver_sent 的 0 覆盖），
# 内部 await 抛出的异常要走 throw 分发，能被生成器自己的 try/except 接住。
import asyncio


async def work(n: int) -> int:
    await asyncio.sleep(0)
    return n * 10


async def agen(n: int):
    v = await work(n)
    print("v", v)
    yield v + 1
    s = await work(n + 1)
    print("s", s)
    yield s + 1


async def failing():
    await asyncio.sleep(0)
    raise ValueError("boom")


async def safe():
    try:
        v = await failing()
        print("never", v)
    except ValueError as e:
        print("caught", e)
    yield 7
    yield 8


async def texts():
    t = await text()
    yield t + "!"


async def text() -> str:
    await asyncio.sleep(0)
    return "hi"


async def items():
    xs = await make()
    yield xs[0] + xs[1]


async def make() -> list:
    await asyncio.sleep(0)
    return [1, 2, 3]


async def main() -> int:
    out = []
    async for x in agen(2):
        out.append(x)
    print(out)

    g = agen(5)
    a = await g.asend(None)
    print("a", a)
    b = await g.asend(None)
    print("b", b)
    await g.aclose()

    vals = []
    async for y in safe():
        vals.append(y)
    print(vals)

    h = safe()
    c = await h.__anext__()
    print("c", c)
    await h.aclose()

    async for s in texts():
        print(s)

    async for n in items():
        print(n)
    return 0


print(asyncio.run(main()))

# --- expect ---
# v 20
# s 30
# [21, 31]
# v 50
# a 51
# s 60
# b 61
# caught boom
# [7, 8]
# caught boom
# c 7
# hi!
# 3
# 0
