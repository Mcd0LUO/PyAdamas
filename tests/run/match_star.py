# 0.626：序列模式的 *rest 之后元素从尾部倒推，rest 的起点是星号前的元素数

def show(v: list) -> None:
    match v:
        case [first, *mid, last]:
            print("A", first, mid, last)
        case _:
            print("A no")

show([10, 20, 30, 40])
show([1, 2])
show([9])

def head_rest(v: list) -> None:
    match v:
        case [a, *rest]:
            print("B", a, rest)
        case _:
            print("B no")

head_rest([1, 2, 3])

def rest_tail(v: list) -> None:
    match v:
        case [*init, x, y]:
            print("C", init, x, y)
        case _:
            print("C no")

rest_tail([1, 2, 3, 4])

def only_star(v: list) -> None:
    match v:
        case [*all]:
            print("D", all)
        case _:
            print("D no")

only_star([7, 8])

# --- expect ---
# A 10 [20, 30] 40
# A 1 [] 2
# A no
# B 1 [2, 3]
# C [1, 2] 3 4
# D [7, 8]
