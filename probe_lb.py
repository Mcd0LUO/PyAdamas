from contextlib import contextmanager
@contextmanager
def cleanup(log: list[str]):
    try:
        yield 7
    finally:
        log.append("clean")
        print("cleanup")
log2: list[str] = []
try:
    with cleanup(log2) as v:
        print("v", v)
        raise RuntimeError("body")
except RuntimeError as e:
    print("caught", e)
print(log2)
