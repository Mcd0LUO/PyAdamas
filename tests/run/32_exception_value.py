import asyncio


def f(n: int) -> int:
    if n < 0:
        raise ValueError("negative")
    return n


def run() -> int:
    try:
        f(-1)
    except ValueError as e:
        print("caught", e)
        print(len(e.args))
        print(e.args[0])
        raise
    return 0


def g() -> int:
    e = ValueError("boom")
    raise e


async def agen():
    try:
        yield 1
        yield 2
    except ValueError as e:
        print("gen caught", e)
    yield 99


async def main() -> int:
    try:
        run()
    except ValueError as e:
        print("outer", e)
    try:
        g()
    except ValueError as e:
        print("re-raised", e)
        print(str(e))

    a = agen()
    v = await a.asend(None)
    print(v)
    w = await a.athrow(ValueError("inject"))
    print(w)
    try:
        x = await a.asend(None)
        print(x)
    except StopAsyncIteration:
        print("agen done")

    fut = asyncio.Future()
    fut.set_exception(ValueError("fboom"))
    try:
        r = await fut
        print(r)
    except ValueError as e:
        print("future", e)
    print(fut.exception())
    return 0


print(asyncio.run(main()))

# --- expect ---
# caught negative
# 1
# negative
# outer negative
# re-raised boom
# boom
# 1
# gen caught inject
# 99
# agen done
# future fboom
# fboom
# 0
