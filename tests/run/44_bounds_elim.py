def pure(xs: list[int]) -> int:
    s = 0
    for x in xs:
        s = s + x
    return s


def rebind(xs: list[int]) -> int:
    s = 0
    for x in xs:
        xs = [1]
        s = s + x
    return s


def grow(xs: list[int]) -> int:
    total = 0
    for x in xs:
        xs.append(x)
        total = total + x
    return total


def mutate(x: int, xs: list[int]) -> int:
    xs.append(x)
    return x


def comp_call(xs: list[int]) -> int:
    ys = [mutate(x, xs) for x in xs]
    return len(ys)


ys = [1, 2, 3]
print(pure(ys))
print(rebind(ys))
print(grow(ys), len(ys))

zs = [4, 5, 6]
print(comp_call(zs), len(zs))

fs: list[float] = [1.5, 2.5, 3.5]
print(sum(fs), min(fs), max(fs))
print(sum([1, 2, 3]), min([3, 1, 2]), max([3, 1, 2]))
print(any([0, 2, 3]), all([1, 2, 3]), any([0, 0]))
print(*[1, 2, 3])

xs2 = [1, 2, 3, 4]
for i in range(len(xs2)):
    xs2[i] = xs2[i] * 2
print(xs2[0], xs2[3])

sq = [x * x for x in range(5)]
print(len(sq), sq[0], sq[4])

# --- expect ---
# 6
# 6
# 6 6
# 3 6
# 7.5 1.5 3.5
# 6 1 3
# True True False
# 1 2 3
# 2 8
# 5 0 16
