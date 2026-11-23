# unknown 域实例作 dict 键 / set 元素（0.435）：按 vtable dunder 槽表调
# __hash__ 与 __eq__，与 CPython 的「等值不同对象命中同一个键」一致。
class K:
    def __init__(self, v: int) -> None:
        self.v = v

    def __hash__(self) -> int:
        return self.v

    def __eq__(self, other: "K") -> bool:
        return self.v == other.v


class E:
    def __init__(self, v: int) -> None:
        self.v = v

    def __eq__(self, other: "E") -> bool:
        return self.v == other.v


def mk_dict() -> dict:
    return {}


def put(d: dict, k, v) -> None:
    d[k] = v


def get(d: dict, k) -> int:
    return d[k]


def has(d: dict, k) -> bool:
    return k in d


def h(x) -> int:
    return hash(x)


k1 = K(1)
k2 = K(1)
k3 = K(2)
d: dict = mk_dict()
put(d, k1, 7)
print("unk_dict", get(d, k2), has(d, k3))
put(d, k2, 8)
print("overwrite", get(d, k1), len(d))
xs: list = [k1]
s: set = set(xs)
s.add(k2)
s.add(k3)
print("unk_set", len(s))
print("unk_hash", h(k1) == h(k2), h(k1) == h(k3))
e1 = E(1)
d2: dict = mk_dict()
ok = 0
try:
    put(d2, e1, 1)
    ok = 1
except TypeError:
    ok = 0
print("unhashable", ok)
td = {k1: 5}
print("typed", td[k2], k2 in td, hash(k1) == hash(k2))

# --- expect ---
# unk_dict 7 False
# overwrite 8 1
# unk_set 2
# unk_hash True False
# unhashable 0
# typed 5 True True
