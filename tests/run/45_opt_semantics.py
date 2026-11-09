def maybe(x: int) -> int:
    z: int
    if x > 0:
        z = 1
    return z


print(maybe(0), maybe(1))


def nested(n: int) -> int:
    total = 0
    for i in range(n):
        for j in range(i + 1):
            if (i + j) % 2 == 0:
                total = total + j
            else:
                total = total + i
    return total


print(nested(5))


def commas(xs: list[int]) -> int:
    s = 0
    for x in xs:
        if x > 0 and x < 10:
            s = s + x
    return s


print(commas([1, -2, 20, 3]))

# --- expect ---
# 0 1
# 30
# 4
