import b
print("load a", b.g(2))

def f(x: int) -> int:
    return b.g(x) + 1
