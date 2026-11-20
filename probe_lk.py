def g():
    yield 1, 2

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

with CM(g()) as v:
    print(v[0], v[1])
