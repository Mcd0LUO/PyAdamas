from contextlib import contextmanager

@contextmanager
def tag(name: str):
    yield name

@contextmanager
def cleanup(log: list[str]):
    try:
        yield 7
    finally:
        log.append("clean")
        print("cleanup")

log: list[str] = []
with tag("x") as v:
    print("t", v)
with cleanup(log) as v:
    print("v", v)
