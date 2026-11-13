import asyncio
import mod


async def use_async() -> int:
    total = 0
    async for t in mod.ticks(3):
        total += len(t)
    v = await mod.answer(41)
    print(v)
    return total


def main() -> None:
    total = 0
    for x in mod.nums(3):
        total += x
    print(total)
    for s in mod.shout("ab"):
        print(s)
    g = mod.nums(2)
    print(next(g))
    print(g.send(None))
    print(asyncio.run(use_async()))


main()

# --- expect ---
# 6
# AB
# 0
# 2
# 42
# 6
