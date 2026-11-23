# 取消语义回归（0.437）：cancel 后 await、cancel 不 await、重复 await、双 task、wait_for 超时。
import asyncio


async def slow(log: list) -> int:
    try:
        await asyncio.sleep(0.02)
        log.append("body")
        return 1
    finally:
        log.append("fin")


async def cancel_then_await() -> str:
    log: list = []
    t = asyncio.create_task(slow(log))
    await asyncio.sleep(0)
    ok = t.cancel()
    log.append("cancel_ret" if ok else "cancel_no")
    try:
        await t
        log.append("no-exc")
    except BaseException:
        log.append("caught")
    log.append("done")
    return ",".join([str(x) for x in log])


async def cancel_no_await() -> int:
    t = asyncio.create_task(slow([]))
    await asyncio.sleep(0)
    ok = t.cancel()
    await asyncio.sleep(0.05)
    return 1 if ok else 0


async def two_cancels() -> int:
    a = asyncio.create_task(slow([]))
    b = asyncio.create_task(slow([]))
    await asyncio.sleep(0)
    ca = a.cancel()
    cb = b.cancel()
    caught = 0
    try:
        await a
    except BaseException:
        caught = caught + 1
    try:
        await b
    except BaseException:
        caught = caught + 1
    return caught + (1 if ca and cb else 0)


async def timeout_demo() -> int:
    log: list = []
    try:
        await asyncio.wait_for(slow(log), 0.001)
        return 0
    except TimeoutError:
        return 1 if log == ["fin"] or log == [] else 2


print("cancel", asyncio.run(cancel_then_await()))
print("no_await", asyncio.run(cancel_no_await()))
print("two", asyncio.run(two_cancels()))
print("timeout", asyncio.run(timeout_demo()))

# --- expect ---
# cancel cancel_ret,fin,caught,done
# no_await 1
# two 3
# timeout 1
