# expect: takes 1 positional argument but 2 were given
def f(a: int, *, b: str) -> int:
    return a

f(1, 2)
