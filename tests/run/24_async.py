import asyncio
from asyncio import gather, run, sleep


async def double(x: int) -> int:
    return x * 2


async def add(a: int, b: int) -> int:
    x = await double(a)
    y = await double(b)
    return x + y


print(asyncio.run(add(3, 4)))


async def fail() -> int:
    await sleep(0.001)
    raise ValueError("boom")


async def catcher() -> str:
    try:
        await fail()
    except ValueError:
        return "caught"
    return "missed"


print(asyncio.run(catcher()))


async def cleanup() -> None:
    try:
        await sleep(0.001)
        print("body")
    finally:
        print("cleanup")


asyncio.run(cleanup())


async def one(x: int) -> int:
    await sleep(0)
    return x


async def many() -> int:
    vals = await gather(one(1), one(2), one(3))
    s = 0
    for v in vals:
        s += v
    return s


print(asyncio.run(many()))


class Worker:
    def __init__(self, n: int) -> None:
        self.n = n

    async def run(self) -> int:
        await sleep(0)
        return self.n + 1


print(asyncio.run(Worker(9).run()))


class Ctx:
    def __enter__(self) -> int:
        print("enter")
        return 1

    def __exit__(self, a, b, c) -> bool:
        print("exit")
        return False


async def guarded() -> int:
    with Ctx():
        await sleep(0)
        return 7


print(asyncio.run(guarded()))


async def text(x: int) -> str:
    await sleep(0)
    return str(x)


async def joined() -> str:
    vals = await asyncio.gather(text(11), text(22))
    return vals[0] + "+" + vals[1]


print(asyncio.run(joined()))


async def root_fail() -> int:
    await sleep(0)
    raise ValueError("root")


try:
    asyncio.run(root_fail())
except ValueError:
    print("outer caught")


async def via_from_import() -> int:
    await sleep(0)
    return 21


print(run(via_from_import()))


async def float_one(x: float) -> float:
    await sleep(0)
    return x


async def float_gather() -> float:
    vals = await gather(float_one(0.5), float_one(1.5))
    return vals[0] + vals[1]


print(asyncio.run(float_gather()))

# --- expect ---
# 14
# caught
# body
# cleanup
# 6
# 10
# enter
# exit
# 7
# 11+22
# outer caught
# 21
# 2.0
