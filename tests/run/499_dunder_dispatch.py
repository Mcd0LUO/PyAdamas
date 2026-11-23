class P:
    def __init__(self, v: int) -> None:
        self.v = v

    def __eq__(self, other: "P") -> bool:
        return self.v == other.v

    def __lt__(self, other: "P") -> bool:
        return self.v < other.v


class Q:
    def __init__(self, v: int) -> None:
        self.v = v


class Bad:
    def __init__(self, v: int) -> None:
        self.v = v

    def __eq__(self, other: "Bad") -> bool:
        if self.v < 0:
            raise ValueError("bad-eq")
        return self.v == other.v


def eq(a, b) -> bool:
    return a == b


def lt(a, b) -> bool:
    return a < b


p1 = P(1)
p2 = P(1)
p3 = P(2)
print("typed", p1 == p2, p1 == p3, p1 < p3, p3 < p1)
xs = [P(3), 1]
a = xs[0]
print("unknown_eq", eq(a, P(3)), eq(a, P(1)), a != P(3), a != P(1))
print("unknown_lt", lt(a, P(4)), lt(a, P(2)))
ps = [P(2), P(1), P(3)]
print("sorted", [p.v for p in sorted(ps)])
print("in", P(2) in ps, P(9) in ps, ps.index(P(1)))
ps.remove(P(2))
print("remove", [p.v for p in ps])
qs = [Q(1), Q(2)]
ok = 0
try:
    sorted(qs)
    ok = 1
except TypeError:
    ok = 0
print("no_lt", ok)
# 异常安全：unknown 域 __eq__ 里 raise，要被外层 except 捕获
bs = [Bad(-1), 1]
b = bs[0]
try:
    eq(b, Bad(1))
    print("bad_eq", "no-raise")
except ValueError as e:
    print("bad_eq", str(e))

# --- expect ---
# typed True False True False
# unknown_eq True False False True
# unknown_lt True False
# sorted [1, 2, 3]
# in True False 1
# remove [1, 3]
# no_lt 0
# bad_eq bad-eq
