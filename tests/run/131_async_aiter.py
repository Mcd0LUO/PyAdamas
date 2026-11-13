# 自定义 __aiter__：async for 接受类实例。

import asyncio


async def countdown(n: int):
    while n > 0:
        yield n
        n = n - 1


class Countdown:
    def __init__(self, n: int) -> None:
        self.n = n

    def __aiter__(self):
        return countdown(self.n)


class Squares:
    def __init__(self) -> None:
        self.i = 0

    def __aiter__(self):
        return self

    async def __anext__(self) -> int:
        if self.i >= 3:
            raise StopAsyncIteration
        self.i = self.i + 1
        return self.i * self.i


async def main() -> None:
    total = 0
    async for v in Countdown(3):
        print("c", v)
        total = total + v
    print("total", total)

    kept = 0
    async for v in Countdown(5):
        if v % 2 == 1:
            continue
        kept = kept + v
        if v == 2:
            break
    print("kept", kept)

    count = 0
    last = 0
    async for v in Squares():
        count = count + 1
        last = v
    print("squares", count, last)

    ran = 0
    async for v in Countdown(0):
        ran = ran + 1
    else:
        print("else", ran)


asyncio.run(main())

# --- expect ---
# c 3
# c 2
# c 1
# total 6
# kept 6
# squares 3 9
# else 0
