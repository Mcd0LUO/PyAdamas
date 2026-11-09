import asyncio


async def gen(n: int):
    i = 0
    while i < n:
        await asyncio.sleep(0)
        yield i
        i += 1


async def main() -> int:
    s = 0
    async for v in gen(3):
        s += v
    return s


print(asyncio.run(main()))


class Ctx:
    async def __aenter__(self) -> int:
        await asyncio.sleep(0)
        print("enter")
        return 5

    async def __aexit__(self, a, b, c) -> bool:
        await asyncio.sleep(0)
        print("exit")
        return False


async def with_body() -> int:
    async with Ctx() as v:
        await asyncio.sleep(0)
        print(v)
        return 7


print(asyncio.run(with_body()))


class Swallow:
    async def __aenter__(self) -> int:
        return 1

    async def __aexit__(self, a, b, c) -> bool:
        print("suppress")
        return True


async def suppressed() -> int:
    async with Swallow():
        await asyncio.sleep(0)
        raise ValueError("x")
    return 9


print(asyncio.run(suppressed()))


async def failing() -> int:
    async with Ctx():
        await asyncio.sleep(0)
        raise ValueError("boom")
    return 0


try:
    asyncio.run(failing())
except ValueError:
    print("caught")


async def agen_finally(n: int):
    try:
        i = 0
        while i < n:
            yield i
            i += 1
    finally:
        print("agen finally")


async def close_iter() -> int:
    count = 0
    async for v in agen_finally(2):
        count += v
    return count


print(asyncio.run(close_iter()))


async def early_break() -> int:
    async for v in agen_finally(5):
        print(v)
        break
    await asyncio.sleep(0)
    return 0


print(asyncio.run(early_break()))


async def empty_ag():
    if False:
        yield 1


async def else_test() -> int:
    async for v in empty_ag():
        print("none")
    else:
        print("empty else")
    return 0


print(asyncio.run(else_test()))


async def count_ag(n: int):
    i = 0
    while i < n:
        yield i
        i += 1


async def break_continue() -> int:
    s = 0
    async for v in count_ag(10):
        if v == 1:
            continue
        if v == 4:
            break
        s += v
    else:
        print("full")
    return s


print(asyncio.run(break_continue()))


async def break_with() -> int:
    i = 0
    while i < 3:
        async with Ctx():
            if i == 1:
                break
        i += 1
    return 0


print(asyncio.run(break_with()))

# --- expect ---
# 3
# enter
# 5
# exit
# 7
# suppress
# 9
# enter
# exit
# caught
# agen finally
# 1
# 0
# agen finally
# 0
# empty else
# 0
# 5
# enter
# exit
# enter
# exit
# 0
