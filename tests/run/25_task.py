import asyncio


async def one(x: int) -> int:
    await asyncio.sleep(0)
    return x


async def main() -> int:
    t = asyncio.create_task(one(5))
    print(t.done())
    v = await t
    print(t.done())
    again = await t
    print(again)
    print(t.done())
    return v


print(asyncio.run(main()))


async def order(name: str) -> str:
    await asyncio.sleep(0)
    return name


async def multi() -> None:
    a = asyncio.create_task(order("a"))
    b = asyncio.create_task(order("b"))
    xb = await b
    xa = await a
    print(xb)
    print(xa)
    again = await b
    print(again)


asyncio.run(multi())


async def gt() -> int:
    a = asyncio.create_task(one(1))
    b = asyncio.create_task(one(2))
    vals = await asyncio.gather(a, b)
    return vals[0] + vals[1]


print(asyncio.run(gt()))


async def boom() -> int:
    await asyncio.sleep(0)
    raise ValueError("t")


async def eh() -> int:
    t = asyncio.create_task(boom())
    try:
        await t
    except ValueError:
        print("task caught")
    return 0


print(asyncio.run(eh()))


async def sleeper() -> int:
    try:
        await asyncio.sleep(10)
        return 1
    except CancelledError:
        print("cancelled")
        return 0


async def cmain() -> int:
    t = asyncio.create_task(sleeper())
    await asyncio.sleep(0.001)
    print(t.cancel())
    v = await t
    return v


print(asyncio.run(cmain()))


async def unstarted() -> int:
    return 7


async def cmain2() -> int:
    t = asyncio.create_task(unstarted())
    t.cancel()
    try:
        await t
    except CancelledError:
        print("cancelled early")
    return 0


print(asyncio.run(cmain2()))


async def parent() -> int:
    try:
        await asyncio.sleep(10)
        return 1
    finally:
        print("parent finally")


async def cmain3() -> int:
    t = asyncio.create_task(parent())
    await asyncio.sleep(0.001)
    print(t.cancel())
    try:
        v = await t
        return v
    except CancelledError:
        print("cancel caught")
    return 0


print(asyncio.run(cmain3()))


async def shared() -> int:
    await asyncio.sleep(0.001)
    return 21


async def wait_shared(t: Task[int], tag: str) -> int:
    v = await t
    print(tag, v)
    return v


async def multi_wait() -> int:
    t = asyncio.create_task(shared())
    a = asyncio.create_task(wait_shared(t, "a"))
    b = asyncio.create_task(wait_shared(t, "b"))
    va = await a
    vb = await b
    return va + vb


print(asyncio.run(multi_wait()))


async def float_task() -> float:
    await asyncio.sleep(0)
    return 2.5


async def wait_float() -> float:
    t = asyncio.create_task(float_task())
    return await t


print(asyncio.run(wait_float()))


async def never() -> int:
    await asyncio.sleep(10)
    return 0


async def watch(t: Task[int], tag: str) -> int:
    try:
        await t
    except CancelledError:
        print("cancel", tag)
    return 0


async def multi_cancel() -> int:
    t = asyncio.create_task(never())
    a = asyncio.create_task(watch(t, "a"))
    b = asyncio.create_task(watch(t, "b"))
    await asyncio.sleep(0.001)
    t.cancel()
    await a
    await b
    return 0


print(asyncio.run(multi_cancel()))

# --- expect ---
# False
# True
# 5
# True
# 5
# b
# a
# b
# 3
# task caught
# 0
# True
# cancelled
# 0
# cancelled early
# 0
# True
# parent finally
# cancel caught
# 0
# a 21
# b 21
# 42
# 2.5
# cancel a
# cancel b
# 0
