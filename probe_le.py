def gen():
    print("in gen")
    yield 1
    print("after yield")

class CM:
    gen: Generator[unknown]
    def __init__(self, g: Generator[unknown]) -> None:
        self.gen = g
    def __enter__(self) -> unknown:
        g: Generator[unknown] = self.gen
        return next(g)
    def __exit__(self, et, ev, tb) -> bool:
        g: Generator[unknown] = self.gen
        try:
            next(g)
        except StopIteration:
            return False
        return False

with CM(gen()) as v:
    print("body", v)
