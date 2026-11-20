class _GenCM:
    gen: Generator[unknown]

    def __init__(self, gen: Generator[unknown]) -> None:
        self.gen = gen

    def __enter__(self) -> unknown:
        g: Generator[unknown] = self.gen
        return next(g)

    def __exit__(self, et, ev, tb) -> bool:
        g: Generator[unknown] = self.gen
        if et is None:
            try:
                next(g)
            except StopIteration:
                return False
            return False
        try:
            g.throw(ev)
        except StopIteration:
            return True
        return False

def _make(func: unknown, args: unknown) -> _GenCM:
    f = func
    n = len(args)
    if n == 0:
        g = f()
    elif n == 1:
        g = f(args[0])
    elif n == 2:
        g = f(args[0], args[1])
    else:
        g = f(args[0], args[1], args[2])
    return _GenCM(g)

def contextmanager(func: unknown):
    def helper(*args) -> _GenCM:
        return _make(func, args)
    return helper

@contextmanager
def quiet():
    try:
        yield
    except ValueError:
        print("swallowed")

with quiet():
    raise ValueError("boom")
print("after")

@contextmanager
def loud():
    yield 1

try:
    with loud() as v:
        print("in", v)
        raise RuntimeError("x")
except RuntimeError as e:
    print("caught", e)
print("done")

cm = quiet()
with cm:
    pass
print("ok2")
