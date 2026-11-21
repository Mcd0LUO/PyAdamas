# 435: async with 的 __aexit__ 异常路径。
# 这两个串（类型名 + 消息）原先存在临时槽里：抛出去时 Raise 是 noreturn，
# longjmp 之后槽走不到释放点，挂起前 push 的临时栈也随挂起丢了 → 每次 224B。
import asyncio


class CM:
    def __init__(self, swallow: bool) -> None:
        self.swallow: bool = swallow

    async def __aenter__(self) -> int:
        return 1

    async def __aexit__(self, t: object, v: object, tb: object) -> bool:
        return self.swallow


async def worker(sw: bool) -> str:
    async with CM(sw):
        raise ValueError("boom")
    return "survived"


async def nested() -> str:
    async with CM(False):
        async with CM(True):
            raise KeyError("inner")
        return "after-inner"


async def main() -> None:
    print(await worker(True))
    try:
        await worker(False)
    except ValueError as e:
        print("caught", e)
    try:
        await nested()
    except KeyError as e:
        print("nested", e)


asyncio.run(main())
# --- expect ---
# survived
# caught boom
