# expect: argument 'x' of 'f' expects 'i8', got 'int'
def f(x: i8) -> i8:
    return x
n: int = 5
print(f(n))
