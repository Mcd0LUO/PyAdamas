# expect: declared to return 'int', got 'None'
def f(flag: bool) -> int:
    if flag:
        return 1
    return
