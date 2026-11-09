# expect: mapping pattern checks duplicate key
def f(d: dict[int, int]) -> None:
    match d:
        case {1: a, 1: b}:
            print("dup")
