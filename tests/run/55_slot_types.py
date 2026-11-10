import math


def total(xs: list[float]) -> float:
    s: float = 0.0
    for x in xs:
        s += x
    return s


def main() -> None:
    d: float = math.sqrt(1.0 * 1.0 + 2.0 * 2.0 + 3.0 * 3.0)
    max_d: float = 0.0
    i: int = 0
    while i < 5:
        if d > max_d:
            max_d = d
        i += 1
    print(f"{d:.12f}", f"{max_d:.12f}")
    print(f"{math.fsum([1e16, 1.0, -1e16]):.12f}")
    print(f"{total([1.0, 2.0, 3.0]):.12f}")


main()

# --- expect ---
# 3.741657386774 3.741657386774
# 1.000000000000
# 6.000000000000
