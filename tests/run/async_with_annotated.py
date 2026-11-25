# B3b（0.533）：标注成 asyncio 原语时 async with / acquire / release 也要能用。
# 过去标注会把类型退化成 unknown，async with 在降级阶段被拒（不标注反而能过）。
import asyncio


async def use_param(lock: asyncio.Lock) -> None:
    async with lock:
        print(0)


async def main() -> None:
    lock: asyncio.Lock = asyncio.Lock()
    async with lock:
        print(1)
    sem: asyncio.Semaphore = asyncio.Semaphore(1)
    async with sem:
        print(2)
    await lock.acquire()
    print(3)
    lock.release()
    await sem.acquire()
    print(4)
    sem.release()
    await use_param(asyncio.Lock())


asyncio.run(main())

# --- expect ---
# 1
# 2
# 3
# 4
# 0
