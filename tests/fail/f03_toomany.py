# expect: takes 1 positional argument but 2 were given
def f(a: int) -> int:
    return a

f(1, 2)
