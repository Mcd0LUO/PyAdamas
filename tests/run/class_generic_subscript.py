# 0.565：类名下标是泛型参数，擦除后仍可构造（不做真泛型）。


class Box:
    def __init__(self, v: int) -> None:
        self.v = v

b = Box[int](3)
print(b.v)

# --- expect ---
# 3
