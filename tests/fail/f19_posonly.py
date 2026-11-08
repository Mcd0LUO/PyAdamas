# expect: is a positional-only argument
def f(a: int, /, b: int) -> int:
    return a + b

f(a=1, b=2)
