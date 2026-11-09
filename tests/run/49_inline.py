def add(a: int, b: int) -> int:
    return a + b


def pick(flag: bool) -> str:
    if flag:
        return "yes"
    return "no"


def make(x: int) -> list[int]:
    y: list[int] = [x, x + 1]
    return y


def use(a: int, b: int) -> int:
    total = 0
    for i in range(b):
        total = add(total, a)
    return total


def bump(a: int) -> int:
    a = a + 1
    return a


def cat(a: str, b: str) -> str:
    s: str = a + b
    return s


def count(n: int) -> int:
    s = 0
    i = 0
    while i < n:
        s = s + i
        i = i + 1
    return s


def fact(n: int) -> int:
    if n <= 1:
        return 1
    return n * fact(n - 1)


def sum3(a: int, b: int, c: int) -> int:
    return add(add(a, b), c)


def noop(x: int) -> None:
    print("noop", x)


print(add(2, 3))
print(pick(True), pick(False))
zs = make(5)
print(zs[0], zs[1])
print(use(3, 4))
print(bump(9), cat("ab", "cd"))
print(count(5), fact(6), sum3(1, 2, 3))
noop(7)

# --- expect ---
# 5
# yes no
# 5 6
# 12
# 10 abcd
# 10 720 6
# noop 7
