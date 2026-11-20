from contextlib import contextmanager

@contextmanager
def cleanup(log: list[str]):
    try:
        yield 7
    finally:
        log.append("clean")
        print("cleanup")

log: list[str] = []
with cleanup(log) as v:
    print("v", v)
print(log)
