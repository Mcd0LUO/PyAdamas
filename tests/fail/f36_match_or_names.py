# expect: alternative patterns bind different names
def f(xs: list[int]) -> None:
    match xs:
        case [a] | [b]:
            print("one")
