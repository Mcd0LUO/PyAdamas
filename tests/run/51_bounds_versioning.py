def sum_row(a: list[list[int]], i: int, n: int) -> int:
    row = a[i]
    s = 0
    for k in range(n):
        s = s + row[k]
    return s


def neg_index(a: list[int], n: int) -> int:
    i = -1
    s = 0
    for k in range(n):
        s = s + a[i]
    return s


def while_sum(a: list[int], i: int, n: int) -> int:
    s = 0
    k = 0
    while k < n:
        s = s + a[i]
        k = k + 1
    return s


def float_inv(a: list[float], i: int, n: int) -> float:
    s = 0.0
    for k in range(n):
        s = s + a[i]
    return s


def mutate(a: list[int], n: int) -> int:
    s = 0
    for k in range(n):
        s = s + a[0]
        a[0] = a[0] + 1
    return s


def zero_loop(a: list[int]) -> int:
    s = 0
    i = 0
    while i < 0:
        s = s + a[0]
        i = i + 1
    return s


def broken(a: list[int]) -> int:
    for k in range(0):
        return a[99]
    return 0


print(sum_row([[1, 2, 3], [4, 5, 6]], 1, 3))
print(neg_index([7, 8, 9], 3))
print(while_sum([10, 20, 30], 2, 4))
print(float_inv([1.5, 2.5], 1, 3))
print(mutate([0], 3))
print(zero_loop([]))
print(broken([]))

# --- expect ---
# 15
# 27
# 120
# 7.5
# 3
# 0
# 0
