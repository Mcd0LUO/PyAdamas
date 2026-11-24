# asyncio.TimeoutError 别名（0.484，pya-lsp 缺口 3）：wait_for 超时抛的是内置
# TimeoutError，`except asyncio.TimeoutError` 必须能捕获（与 CPython 3.11+ 一致：
# asyncio.TimeoutError 就是内置 TimeoutError 的同义词）。
import asyncio

async def slow() -> None:
    await asyncio.sleep(0.05)

async def probe(sec: float) -> str:
    try:
        await asyncio.wait_for(slow(), sec)
        return "no-timeout"
    except asyncio.TimeoutError:
        return "asyncio.TimeoutError"

async def main() -> None:
    print(await probe(0.01))
    print(await probe(1.0))
    # 内置名与模块别名是同一个异常类型：反过来的捕获顺序也一样命中
    try:
        await asyncio.wait_for(slow(), 0.01)
    except TimeoutError:
        print("builtin TimeoutError")

asyncio.run(main())
# --- expect ---
# asyncio.TimeoutError
# no-timeout
# builtin TimeoutError
