import asyncio


async def agen(n: int):
    i = 0
    try:
        while i < n:
            v = yield i
            if v is None:
                i += 1
            else:
                i = v
    finally:
        print("agen finally")


async def main() -> int:
    g = agen(5)
    a = await g.asend(None)
    print(a)
    b = await g.asend(4)
    print(b)
    try:
        c = await g.asend(None)
        print(c)
    except StopAsyncIteration:
        print("stop")

    h = agen(3)
    x = await h.asend(None)
    print(x)
    await h.aclose()
    print("closed active")

    z = agen(2)
    await z.aclose()
    print("closed fresh")

    q = agen(2)
    v1 = await q.__anext__()
    print(v1)
    v2 = await q.__anext__()
    print(v2)
    try:
        v3 = await q.__anext__()
        print(v3)
    except StopAsyncIteration:
        print("stop2")

    w = agen(1)
    w1 = await w.asend(None)
    print(w1)
    try:
        w2 = await w.asend(None)
        print(w2)
    except StopAsyncIteration:
        print("done")
    await w.aclose()
    print("closed done")
    return 0


print(asyncio.run(main()))

# --- expect ---
# 0
# 4
# agen finally
# stop
# 0
# agen finally
# closed active
# closed fresh
# 0
# 1
# agen finally
# stop2
# 0
# agen finally
# done
# closed done
# 0
