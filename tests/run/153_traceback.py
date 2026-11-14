# 未捕获异常应在 stderr 打印函数级 traceback（含函数名与行号）。
# 父进程把自己作为子进程重新执行，让子进程在 rec() 里抛出未捕获异常。
import asyncio
import subprocess
import sys


def rec(n: int) -> int:
    if n == 0:
        raise ValueError("tb")
    return rec(n - 1)


def call() -> int:
    return rec(2)


async def async_inner() -> int:
    raise ValueError("ab")


async def async_outer() -> int:
    return await async_inner()


async def async_main() -> int:
    return await async_outer()


def gen() -> int:
    yield 1
    raise ValueError("g")


def child(flag: str) -> str:
    r = subprocess.run([sys.argv[0], flag], capture_output=True, text=True)
    return r.stderr


def parent() -> int:
    e = child("--traceback-child")
    print("Traceback (most recent call last):" in e)
    print(" in rec" in e)
    print("ValueError: tb" in e)
    e2 = child("--traceback-async")
    print(" in async_outer" in e2)
    print(" in async_inner" in e2)
    e3 = child("--traceback-gen")
    print(" in gen" in e3)
    return 0


if "--traceback-child" in sys.argv:
    print(call())
elif "--traceback-async" in sys.argv:
    asyncio.run(async_main())
elif "--traceback-gen" in sys.argv:
    g = gen()
    print(next(g))
    print(next(g))
else:
    print(parent())

# --- expect ---
# True
# True
# True
# True
# True
# True
# 0
