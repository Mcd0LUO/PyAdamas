from contextlib import contextmanager
@contextmanager
def tag(name: str):
    print("enter", name)
    yield name
    print("exit", name)
with tag("x") as v:
    print("body", v)
with tag("a") as a:
    with tag("b") as b:
        print("nested", a, b)
@contextmanager
def pair():
    yield 1, 2
with pair() as v:
    print("pair", v[0], v[1])
@contextmanager
def add(a: int, b: int):
    yield a + b
with add(2, 3) as s:
    print("sum", s)
@contextmanager
def many(a: int, b: int, c: int) -> int:
    yield a + b + c
with many(1, 2, 3) as s:
    print("many", s)
