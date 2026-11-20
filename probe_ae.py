def gen():
    try:
        yield 1
    except ValueError as e:
        print("caught", e)
        yield 2

def thrower(g: Generator[unknown], e: unknown) -> bool:
    try:
        g.throw(e)
    except StopIteration:
        return True
    return False

it = gen()
print(next(it))
print(thrower(it, ValueError))

class CM:
    def __enter__(self) -> int:
        return 1
    def __exit__(self, et, ev, tb) -> bool:
        print("et is ValueError", et is ValueError)
        print("str et", str(et), "str ev", str(ev))
        return False

with CM():
    raise ValueError("boom")
print("after")
