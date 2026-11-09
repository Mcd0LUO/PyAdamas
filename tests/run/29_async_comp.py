import asyncio


async def agen(n: int):
    i = 0
    while i < n:
        await asyncio.sleep(0)
        yield i
        i += 1


async def sgen():
    yield "a"
    await asyncio.sleep(0)
    yield "bb"
    await asyncio.sleep(0)
    yield "ccc"


async def badgen():
    await asyncio.sleep(0)
    yield 1
    raise RuntimeError("genboom")


async def catchem() -> int:
    try:
        xs = [x async for x in badgen()]
        return len(xs)
    except RuntimeError:
        return 7


async def main() -> int:
    xs = [x * 2 async for x in agen(4)]
    print(xs[0])
    print(xs[3])
    print(len(xs))
    evens = [x async for x in agen(6) if x % 2 == 0]
    print(len(evens))
    print(evens[2])
    ss = {s async for s in sgen()}
    print(len(ss))
    print("bb" in ss)
    dd = {s: len(s) async for s in sgen()}
    print(dd["ccc"])
    nested = [y async for x in agen(2) for y in [x, x + 10]]
    print(nested[1])
    print(nested[3])
    mixed = [y for x in [1, 2, 3] async for y in agen(x) if y < 2]
    print(len(mixed))
    sync_ok = [c for c in "ab"]
    print(sync_ok[1])
    return len(xs)


print(asyncio.run(main()))
print(asyncio.run(catchem()))

# --- expect ---
# 0
# 6
# 4
# 3
# 4
# 3
# True
# 3
# 10
# 11
# 5
# b
# 4
# 7
