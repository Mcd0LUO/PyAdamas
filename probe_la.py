from contextlib import contextmanager
@contextmanager
def tag(name: str):
    yield name
with tag("x") as v:
    print("body", v)
with tag("y") as v:
    print("body", v)
