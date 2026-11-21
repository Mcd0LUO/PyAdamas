# expect: cannot assign 'big' to 'str' variable 'c'
def f(flag: bool) -> int:
    if flag:
        c: str = "x"
    c: big = 7
    return 1 if c == 0 else 0
