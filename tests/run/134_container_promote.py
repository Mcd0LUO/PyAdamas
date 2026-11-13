# 容器元素隐式数值提升：list / dict 的 int → float（赋值与实参）。

from functools import reduce


def total(xs: list[float]) -> float:
    s = 0.0
    for x in xs:
        s = s + x
    return s


def getf(d: dict[str, float]) -> float:
    return d["a"]


def addf(a: float, b: float) -> float:
    return a + b


ys: list[float] = [1, 2, 3]
print(total([1, 2]))
print(total(ys))
print(getf({"a": 1}) + 0.5)
print(reduce(addf, [1, 2, 3], 0.5))

# --- expect ---
# 3.0
# 6.0
# 1.5
# 6.5
