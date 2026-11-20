from contextlib import contextmanager
@contextmanager
def pair():
    yield [1, 2]
@contextmanager
def cleanup(log: list[str]):
    try:
        yield 7
    finally:
        log.append("clean")
        print("cleanup")
log: list[str] = []
with pair() as v:
    print(v[0], v[1])
with cleanup(log) as v:
    print("v", v)
