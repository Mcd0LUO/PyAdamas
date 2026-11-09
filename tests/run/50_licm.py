def nested(a: int, b: int, n: int) -> int:
    s = 0
    for i in range(n):
        for j in range(n):
            s = s + a * b + i
    return s


def shadow(n: int) -> int:
    x = 1
    s = 0
    for i in range(n):
        s = s + x
        x = x + 1
    return s


def zero_iter(a: list[int], n: int) -> int:
    s = 0
    for i in range(n):
        s = s + a[0]
    return s


def guard(n: int) -> int:
    x = 5
    s = 0
    i = 0
    while i < n:
        s = s + x * 3
        i = i + 1
    return s


def with_try(n: int) -> int:
    x = 2
    s = 0
    for i in range(n):
        try:
            s = s + x
        finally:
            x = x + 1
    return s


def gen(n: int):
    x = 3
    for i in range(n):
        yield x * i


print(nested(3, 4, 5))
print(shadow(6))
print(zero_iter([], 0), zero_iter([7], 3))
print(guard(4))
print(with_try(4))
acc = 0
for v in gen(4):
    acc = acc + v
print(acc)

# --- expect ---
# 350
# 21
# 0 21
# 60
# 14
# 18
