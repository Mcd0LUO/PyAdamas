class _GenCM:
    gen: generator[unknown]

    def __init__(self, gen: generator[unknown]) -> None:
        self.gen = gen

    def __enter__(self) -> unknown:
        return next(self.gen)

    def __exit__(self, et, ev, tb) -> bool:
        try:
            next(self.gen)
        except StopIteration:
            return False
        return False

class contextmanager:
    func: unknown

    def __init__(self, func: unknown) -> None:
        self.func = func

    def __call__(self, *args) -> _GenCM:
        f = self.func
        n = len(args)
        if n == 0:
            return _GenCM(f())
        elif n == 1:
            return _GenCM(f(args[0]))
        else:
            return _GenCM(f(args[0], args[1]))

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
