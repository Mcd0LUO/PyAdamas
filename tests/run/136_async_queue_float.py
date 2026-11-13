# asyncio.Queue 的 float 元素：put/get 必须按裸值处理，
# 不能把 double 位模式当成引用去做 RC。

import asyncio


async def main() -> None:
    q: asyncio.Queue[float] = asyncio.Queue()
    await q.put(2.5)
    await q.put(1)
    print(await q.get())
    print(await q.get())

    s: asyncio.Queue[str] = asyncio.Queue()
    await s.put("a")
    await s.put("b")
    print(await s.get(), await s.get())

    n: asyncio.Queue[float] = asyncio.Queue(2)
    n.put_nowait(3.5)
    print(n.get_nowait())

    old = asyncio.Queue()
    await old.put("x")
    print(await old.get())


asyncio.run(main())

# --- expect ---
# 2.5
# 1.0
# a b
# 3.5
# x
