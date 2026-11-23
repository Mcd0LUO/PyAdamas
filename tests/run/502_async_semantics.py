# 协程/异步生成器语义回归（0.437）：async def/await、gather/create_task、异常、
# async for / __anext__ / aclose、取消与超时的 finally。
import asyncio


async def double(x: int) -> int:
    return x * 2


async def add(a: int, b: int) -> int:
    x = await double(a)
    await asyncio.sleep(0)
    return x + b


async def one(x: int) -> int:
    await asyncio.sleep(0)
    return x


async def many() -> int:
    vals = await asyncio.gather(one(1), one(2), one(3))
    s = 0
    for v in vals:
        s = s + v
    return s


async def task_demo() -> int:
    t1 = asyncio.create_task(one(10))
    t2 = asyncio.create_task(one(20))
    a = await t1
    b = await t2
    again = await t1
    return a + b + again


async def fail() -> int:
    await asyncio.sleep(0)
    raise ValueError("boom")


async def catcher() -> str:
    try:
        await fail()
    except ValueError as e:
        return "caught:" + str(e)
    return "missed"


async def agen(n: int):
    i = 0
    while i < n:
        yield i * 2
        i = i + 1


async def collect() -> int:
    out = []
    async for v in agen(3):
        out.append(v)
    return len(out)


async def manual() -> int:
    g = agen(2)
    a = await g.__anext__()
    b = await g.__anext__()
    return a + b


async def partial() -> int:
    g = agen(3)
    v = await g.__anext__()
    await g.aclose()
    return v


async def slow() -> int:
    try:
        await asyncio.sleep(0.02)
        return 1
    finally:
        pass


async def cancel_no_await() -> int:
    t = asyncio.create_task(slow())
    await asyncio.sleep(0)
    ok = t.cancel()
    await asyncio.sleep(0.05)
    return 1 if ok else 0


async def timeout_only() -> int:
    try:
        await asyncio.wait_for(slow(), 0.001)
        return 0
    except TimeoutError:
        return 1


print("add", asyncio.run(add(3, 4)))
print("gather", asyncio.run(many()))
print("tasks", asyncio.run(task_demo()))
print("caught", asyncio.run(catcher()))
top = 0
try:
    asyncio.run(fail())
except ValueError as e:
    top = str(e)
print("top", top)
print("asyncgen", asyncio.run(collect()), asyncio.run(manual()), asyncio.run(partial()))
print("cancel", asyncio.run(cancel_no_await()))
print("timeout", asyncio.run(timeout_only()))

# --- expect ---
# add 10
# gather 6
# tasks 40
# caught caught:boom
# top boom
# asyncgen 3 2 0
# cancel 1
# timeout 1
