# expect: 'match' statement is not supported
def f(x: int) -> int:
    match x:
        case 1:
            return 1
        case _:
            return 0
