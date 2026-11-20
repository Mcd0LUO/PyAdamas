class CM:
    gen: Generator[unknown]
    def __init__(self, x: Generator[unknown]) -> None:
        self.gen = x
    def __enter__(self) -> unknown:
        g2: Generator[unknown] = self.gen
        return next(g2)
    def __exit__(self, et, ev, tb) -> bool:
        g2: Generator[unknown] = self.gen
        for _ in g2:
            pass
        return False

def make_cm(f: unknown, args: unknown) -> CM:
    n = len(args)
    if n == 0:
        g = f()
    else:
        g = f(args[0])
    return CM(g)

def contextmanager(func: unknown):
    def helper(*args) -> CM:
        return make_cm(func, args)
    return helper

@contextmanager
def pair():
    yield 1, 2

with pair() as v:
    print(v[0], v[1])
