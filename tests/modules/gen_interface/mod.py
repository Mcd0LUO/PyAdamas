import asyncio


def nums(n: int):
    for i in range(n):
        yield i * 2


def shout(name: str):
    yield name.upper()


async def answer(x: int) -> int:
    await asyncio.sleep(0)
    return x + 1


async def ticks(n: int):
    for i in range(n):
        await asyncio.sleep(0)
        yield "t" + str(i)
