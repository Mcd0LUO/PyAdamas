from contextlib import contextmanager
@contextmanager
def pair():
    yield 1, 2
with pair() as v:
    print(v[0], v[1])
