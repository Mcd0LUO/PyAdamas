import asyncio


async def setter(f: Future[int], v: int) -> None:
    await asyncio.sleep(0.001)
    f.set_result(v)


async def future_main() -> int:
    f = asyncio.Future()
    asyncio.create_task(setter(f, 41))
    x = await f
    print(f.done())
    print(f.result())
    print(f.exception())
    return x + 1


print(asyncio.run(future_main()))


async def waiter(f) -> int:
    return await f


async def two_waiters() -> int:
    f = asyncio.Future()
    t1 = asyncio.create_task(waiter(f))
    t2 = asyncio.create_task(waiter(f))
    await asyncio.sleep(0.001)
    f.set_result(7)
    a = await t1
    b = await t2
    return a + b


print(asyncio.run(two_waiters()))


async def cancel_future() -> int:
    f = asyncio.Future()
    print(f.cancel())
    print(f.cancelled())
    print(f.done())
    try:
        x = await f
        return x
    except CancelledError:
        print("future cancelled")
    return 0


print(asyncio.run(cancel_future()))


async def one(x: int) -> int:
    await asyncio.sleep(0)
    return x


async def boom() -> int:
    await asyncio.sleep(0)
    raise ValueError("x")


async def task_results() -> int:
    t = asyncio.create_task(one(5))
    e = asyncio.create_task(boom())
    await asyncio.sleep(0.001)
    print(t.done())
    print(t.result())
    print(t.cancelled())
    print(e.done())
    print(e.exception())
    return 0


print(asyncio.run(task_results()))


async def shared_future() -> int:
    f = asyncio.Future()
    t1 = asyncio.create_task(waiter(f))
    t2 = asyncio.create_task(waiter(f))
    await asyncio.sleep(0.001)
    t1.cancel()
    try:
        a = await t1
        print(a)
    except CancelledError:
        print("t1 cancelled")
    try:
        b = await t2
        print(b)
    except CancelledError:
        print("t2 cancelled")
    print(f.cancelled())
    return 0


print(asyncio.run(shared_future()))


async def child_task():
    try:
        await asyncio.sleep(10)
    finally:
        print("b cleanup")


async def canceller() -> int:
    t = asyncio.create_task(child_task())
    try:
        v = await t
        return v
    finally:
        print("a cleanup")


async def cascade() -> int:
    ta = asyncio.create_task(canceller())
    await asyncio.sleep(0.001)
    ta.cancel()
    try:
        v = await ta
        return v
    except CancelledError:
        print("outer cancelled")
    return 0


print(asyncio.run(cascade()))


async def child(name: str) -> int:
    try:
        await asyncio.sleep(10)
        return 1
    finally:
        print("child cleanup " + name)


async def grouped() -> int:
    vals = await asyncio.gather(child("a"), child("b"))
    return vals[0]


async def gather_cascade() -> int:
    t = asyncio.create_task(grouped())
    await asyncio.sleep(0.001)
    t.cancel()
    try:
        v = await t
        return v
    except CancelledError:
        print("gather cancelled")
    return 0


print(asyncio.run(gather_cascade()))


async def fd_test() -> int:
    await asyncio.wait_writable(1)
    print("writable")
    try:
        await asyncio.wait_readable(999999)
    except OSError:
        print("bad fd")
    await asyncio.gather(asyncio.wait_writable(1), asyncio.sleep(0.001))
    print("both")
    t = asyncio.create_task(asyncio.wait_writable(1))
    t.cancel()
    try:
        v = await t
        return v
    except CancelledError:
        print("fd cancelled")
    return 0


print(asyncio.run(fd_test()))

# --- expect ---
# True
# 41
# None
# 42
# 14
# True
# True
# True
# future cancelled
# 0
# True
# 5
# False
# True
# x
# 0
# t1 cancelled
# t2 cancelled
# True
# 0
# b cleanup
# a cleanup
# outer cancelled
# 0
# child cleanup a
# child cleanup b
# gather cancelled
# 0
# writable
# bad fd
# both
# fd cancelled
# 0
