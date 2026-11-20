def gen():
    print("in gen")
    yield 1
    print("after yield")

def resume(g: Generator[unknown]) -> bool:
    try:
        next(g)
    except StopIteration:
        return True
    return False

class CM:
    gen: Generator[unknown]
    def __init__(self, g: Generator[unknown]) -> None:
        self.gen = g
    def __enter__(self) -> unknown:
        g: Generator[unknown] = self.gen
        return next(g)
    def __exit__(self, et, ev, tb) -> bool:
        g: Generator[unknown] = self.gen
        if et is None:
            if resume(g):
                return False
            raise RuntimeError("nope")
        g.close()
        return False

with CM(gen()) as v:
    print("body", v)
with CM(gen()) as v:
    print("body2", v)
