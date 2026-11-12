# 挂起点位置：yield/await 可以是语句里第一个求值的表达式（return 值、
# 条件、二元表达式左操作数），恢复后再计算其余部分。
import asyncio


def gen() -> Generator[int]:
    a: int = yield 1
    if (yield 2) > 0:
        a += 10
    return (yield 3) + a


g = gen()
print(next(g))
print(g.send(5))
print(g.send(1))
try:
    print(g.send(4))
except StopIteration:
    print("stop")


async def work(x: int) -> int:
    return x * 2


async def main() -> None:
    r: int = await work(3)
    if (await work(1)) > 0:
        r += 1
    v: int = (await work(2)) + r
    print(v, r)
    if (await work(0)) > 0 or v > 0:
        print("branch")


asyncio.run(main())

# --- expect ---
# 1
# 2
# 3
# stop
# 11 7
# branch
