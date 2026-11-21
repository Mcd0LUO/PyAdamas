# 451: 协程帧的 gcrel 释放槽之后必须清零。
# pa_gen_slot_release 只在 GC 清扫阶段清槽，普通销毁路径上槽里会留一个
# 已释放的指针。协程的异常收尾（coro_step 的 setjmp 兜底）与正常收尾
# （release_slots）都会跑一遍 gcrel，第二次就撞野指针：
#   pyadamas: runtime error: refcount decrement on a freed object
import asyncio


class CM:
    async def __aenter__(self) -> int:
        return 1

    async def __aexit__(self, a: object, b: object, c: object) -> bool:
        return False

    async def close(self) -> int:
        return 3


async def plain() -> str:
    cm = CM()
    v: int = await cm.close()
    await asyncio.sleep(10)
    return "plain"


async def with_finally() -> str:
    cm = CM()
    v: int = await cm.close()
    try:
        await asyncio.sleep(10)
    finally:
        await cm.close()
    return "finally"


async def ctx() -> str:
    cm = CM()
    async with CM() as v:
        await asyncio.sleep(10)
    return "ctx"


async def run_one(coro: Coroutine[Any, Any, str]) -> None:
    task = asyncio.create_task(coro)
    await asyncio.sleep(0)
    task.cancel()
    try:
        await task
    except BaseException:
        print("cancelled")
    await asyncio.sleep(0)


async def main() -> None:
    await run_one(plain())
    await run_one(with_finally())
    await run_one(ctx())
    print("end")


asyncio.run(main())
# --- expect ---
# cancelled
# cancelled
# cancelled
# end
