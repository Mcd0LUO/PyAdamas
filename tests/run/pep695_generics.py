# 0.566：PEP 695 的泛型语法（3.12）—— 类型参数**擦除**，不做真泛型。


class Box[T]:
    def __init__(self, v: int) -> None:
        self.v = v

    def get[S](self) -> int:
        return self.v


class Pair[T: int, *Ts, **P]:
    pass


def first[T](xs: list[int]) -> int:
    return xs[0]


b = Box[int](3)
print(b.v, b.get())
print(first([7, 8]))
print(Pair())

# --- expect ---
# 3 3
# 7
# <Pair object>
