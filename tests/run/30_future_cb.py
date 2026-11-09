import asyncio


def first(f: Future[int]):
    print("first", f.done())


def second(f: Future[int]):
    print("second", f.done())


def on_cancel(f: Future[int]):
    print("cancel cb", f.cancelled())


def make_cb(tag: str):
    def cb(f: Future[int]):
        print(tag, f.done())

    return cb


async def main() -> int:
    a = asyncio.Future()
    a.add_done_callback(first)
    a.add_done_callback(second)
    a.set_result(3)
    v = await a
    print(v)

    b = asyncio.Future()
    b.add_done_callback(on_cancel)
    b.cancel()
    print(b.cancelled())

    c = asyncio.Future()
    c.set_exception("boom")
    print(c.done())
    print(c.exception())
    try:
        x = await c
        print(x)
    except Exception as e:
        print("caught", e)

    d = asyncio.Future()
    d.set_result(7)
    d.add_done_callback(first)
    print(d.result())

    e = asyncio.Future()
    e.add_done_callback(make_cb("closure"))
    e.set_result(11)
    return 0


print(asyncio.run(main()))

# --- expect ---
# first True
# second True
# 3
# cancel cb True
# True
# True
# boom
# caught boom
# first True
# 7
# closure True
# 0
