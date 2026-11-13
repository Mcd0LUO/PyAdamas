# 126：vtable 槽 dunder 的装饰器分派（__init__ / __enter__ / __exit__ /
# __call__ / async with）
import asyncio


def named_init(f):
    def wrapper(self, name: str) -> None:
        print("init", name)
        f(self, name)
    return wrapper


def traced_enter(f):
    def wrapper(self):
        print("enter")
        return f(self) + 1
    return wrapper


def traced_exit(f):
    def wrapper(self, a, b, c):
        print("exit")
        return f(self, a, b, c)
    return wrapper


class Ctx:
    @named_init
    def __init__(self, name: str) -> None:
        self.name = name

    @traced_enter
    def __enter__(self) -> int:
        return len(self.name)

    @traced_exit
    def __exit__(self, a, b, c) -> bool:
        return self.name == "swallow"


c = Ctx("abc")
print(c.name)


def body() -> int:
    with Ctx("ab") as v:
        print(v)
    try:
        with Ctx("cd") as v2:
            print(v2)
            raise ValueError("boom")
    except ValueError as e:
        print("caught", e)
    with Ctx("swallow") as v3:
        print(v3)
        raise ValueError("ignored")
    return 0


print(body())


def counted_call(f):
    def wrapper(self, a: int, b: int) -> int:
        print("call", a, b)
        return f(self, a, b)
    return wrapper


class Adder:
    def __init__(self, bias: int) -> None:
        self.bias = bias

    @counted_call
    def __call__(self, a: int, b: int) -> int:
        return a + b + self.bias


class Mul:
    def __call__(self, a: int) -> int:
        return a * 3


adder = Adder(100)
print(adder(1, 2))
m = Mul()
print(m(5))
print(Adder(10)(1, 1))


def traced_aenter(f):
    async def wrapper(self):
        print("aenter")
        v = await f(self)
        return v + 1
    return wrapper


def traced_aexit(f):
    async def wrapper(self, a, b, c):
        print("aexit")
        return await f(self, a, b, c)
    return wrapper


class WrappedACtx:
    @traced_aenter
    async def __aenter__(self) -> int:
        return 4

    @traced_aexit
    async def __aexit__(self, a, b, c) -> bool:
        return False


async def amain() -> int:
    async with WrappedACtx() as v:
        print(v)
    return 0


print(asyncio.run(amain()))

# --- expect ---
# init abc
# abc
# init ab
# enter
# 3
# exit
# init cd
# enter
# 3
# exit
# caught boom
# init swallow
# enter
# 8
# exit
# 0
# call 1 2
# 103
# 15
# call 1 1
# 12
# aenter
# 5
# aexit
# 0
