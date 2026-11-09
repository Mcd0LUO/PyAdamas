# expect: has no __match_args__
class A:
    x: int

    def __init__(self, x: int) -> None:
        self.x = x


def f(a: A) -> None:
    match a:
        case A(1):
            print("one")
