# 跨多个挂起点仍活跃的值必须落槽：await 链与生成器都要求「挂起前存、
# 续跑后取」。0.163 之前活跃性把挂起块的续跑块当成了没有后继，第二个
# 挂起点之后读到的就是上一轮留下的垃圾（表现为 use-after-free 或段错误）。
import asyncio


async def add(a: int, b: int) -> int:
    return a + b


async def main() -> None:
    r = await add(await add(1, 2), await add(3, 4))
    print("nested", r)
    total = 0
    i = 0
    while i < 3:
        total = total + await add(i, 10)
        i += 1
    print("loop", total)


def gen(n: int) -> int:
    acc = n
    for i in range(3):
        got = yield acc + i
        acc = acc + got
    return acc


def drive() -> None:
    g = gen(100)
    print("g1", next(g))
    print("g2", g.send(1))
    print("g3", g.send(2))
    try:
        g.send(3)
    except StopIteration:
        print("stop")


asyncio.run(main())
drive()

# --- expect ---
# nested 10
# loop 33
# g1 100
# g2 102
# g3 105
# stop
