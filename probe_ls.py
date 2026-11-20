def g1():
    yield 1, 2

def g2():
    try:
        yield 7
    finally:
        print("cleanup")

class CM:
    gen: Generator[unknown]
    def __init__(self, x: Generator[unknown]) -> None:
        self.gen = x
    def __enter__(self) -> unknown:
        g3: Generator[unknown] = self.gen
        return next(g3)
    def __exit__(self, et, ev, tb) -> bool:
        g3: Generator[unknown] = self.gen
        for _ in g3:
            pass
        return False

with CM(g1()) as v:
    print(v[0], v[1])
with CM(g2()) as v:
    print(v)
