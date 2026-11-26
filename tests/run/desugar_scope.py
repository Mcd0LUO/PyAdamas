# 0.637：脱糖的作用域扫描要看见 match 捕获、walrus 与 del
def my_range(n: int) -> list[int]:
    return [n]


def walrus_shadow() -> None:
    if (range := my_range) is not None:
        for i in range(3):
            print("shadowed", i)


walrus_shadow()


def match_capture(v: list) -> None:
    match v:
        case [first, *rest]:
            print("captured", first, rest)


match_capture([1, 2, 3])


def del_shadow() -> None:
    range = my_range
    del range
    print("after del ok")


del_shadow()

# --- expect ---
# shadowed 3
# captured 1 [2, 3]
# after del ok
