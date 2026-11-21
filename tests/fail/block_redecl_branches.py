# expect: cannot assign 'int' to 'str' variable 'c'
def f(flag: bool) -> int:
    if flag:
        c: str = "x"
    else:
        c: int = 1
    return 0
