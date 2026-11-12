# 生成器/协程/异步生成器异常逃出、close 的 finally 抛异常：帧里持有引用的
# 槽要当场释放（对象仍被引用时不能等销毁），输出与 CPython 一致。
import asyncio


def gen(n: int) -> Generator[int]:
    big: list[int] = list(range(n % 5 + 2))
    text: str = str(n)
    if n % 2 == 0:
        raise ValueError(text)
    yield len(big)


kept: list[Generator[int]] = []
i: int = 0
while i < 400:
    g = gen(i)
    try:
        next(g)
    except ValueError:
        kept.append(g)
    i += 1
print(len(kept))


def closer(n: int) -> Generator[int]:
    big: list[int] = list(range(3))
    text: str = str(n)
    try:
        yield len(big)
    finally:
        raise RuntimeError(text)


kept2: list[Generator[int]] = []
j: int = 0
while j < 300:
    g2 = closer(j)
    next(g2)
    try:
        g2.close()
    except RuntimeError:
        kept2.append(g2)
    j += 1
print(len(kept2))


async def work(n: int) -> int:
    big: list[int] = list(range(3))
    if n % 2 == 0:
        raise ValueError(str(n))
    return len(big)


async def agen(n: int) -> AsyncGenerator[int]:
    big: list[int] = list(range(3))
    if n % 2 == 0:
        raise ValueError(str(n))
    yield len(big)


async def main() -> None:
    keptc: list[Coroutine[int]] = []
    kepta: list[AsyncGenerator[int]] = []
    k: int = 0
    while k < 300:
        c = work(k)
        try:
            await c
        except ValueError:
            keptc.append(c)
        a = agen(k)
        try:
            await a.__anext__()
        except ValueError:
            kepta.append(a)
        k += 1
    print(len(keptc), len(kepta))


asyncio.run(main())

# --- expect ---
# 200
# 300
# 150 150
