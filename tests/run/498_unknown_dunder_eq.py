class P:
    def __init__(self, v: int) -> None:
        self.v = v

    def __eq__(self, other: "P") -> bool:
        return self.v == other.v


class Q:
    def __init__(self, v: int) -> None:
        self.v = v


class R:
    def __init__(self, v: int) -> None:
        self.v = v

    def __eq__(self, other: "R") -> bool:
        if self.v < 0:
            return self.v == other.v
        return self.v == other.v


def eq(a, b) -> bool:
    return a == b


p1 = P(1)
p2 = P(1)
p3 = P(2)
print("typed", p1 == p2, p1 == p3)
xs = [P(1), 1]
a = xs[0]
print("unknown_eq", eq(a, P(1)), eq(a, P(2)), eq(a, a))
print("unknown_ne", a != P(2), a != P(1))
ys = [Q(1), 1]
q1 = ys[0]
print("no_dunder", eq(q1, Q(1)), eq(q1, q1))
print("nested", eq([P(1)], [P(1)]), eq([P(1)], [P(2)]))
rs = [R(5), 1]
r1 = rs[0]
print("rich_eq", eq(r1, R(5)), eq(r1, R(6)))

# --- expect ---
# typed True False
# unknown_eq True False True
# unknown_ne True False
# no_dunder False True
# nested True False
# rich_eq True False
