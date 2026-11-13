# 异步生成器表达式；前置引用的异步生成器函数。

import asyncio


async def main() -> None:
    # 前置引用：agen 在 main 之后定义。
    total = 0
    async for v in agen(3):
        total = total + v
    print("total", total)

    doubled = 0
    async for v in (x * 2 async for x in agen(3)):
        doubled = doubled + v
    print("doubled", doubled)

    filtered = 0
    async for v in (x async for x in agen(5) if x % 2 == 1):
        filtered = filtered + v
    print("filtered", filtered)

    g = (x + 10 async for x in agen(2))
    a = await g.__anext__()
    b = await g.__anext__()
    print("manual", a, b)


async def agen(n: int):
    i = 0
    while i < n:
        i = i + 1
        yield i


asyncio.run(main())

# --- expect ---
# total 6
# doubled 12
# filtered 9
# manual 11 12
