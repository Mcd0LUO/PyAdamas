# expect: missing required argument: 'b'
def g(a: int, b: int) -> int:
    return a + b

g(1)
