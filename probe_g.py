from typing import cast

class _GenCM:
    gen: unknown

    def __init__(self, gen: unknown) -> None:
        self.gen = gen

    def __enter__(self) -> unknown:
        g = cast(generator[unknown], self.gen)
        return next(g)

    def __exit__(self, et, ev, tb) -> bool:
        g = cast(generator[unknown], self.gen)
        try:
            next(g)
        except StopIteration:
            return False
        return False

def _make(func: unknown, args: unknown) -> _GenCM:
    f = func
    n = len(args)
    if n == 0:
        g = f()
    elif n == 1:
        g = f(args[0])
    else:
        g = f(args[0], args[1])
    return _GenCM(g)

def contextmanager(func: unknown):
    def helper(*args) -> _GenCM:
        return _make(func, args)
    return helper

@contextmanager
def tag(name: str):
    print("start", name)
    yield name
    print("end", name)

with tag("x") as v:
    print("body", v)

@contextmanager
def two(a: int, b: int):
    yield a + b

with two(1, 2) as s:
    print("sum", s)
print("ok")
