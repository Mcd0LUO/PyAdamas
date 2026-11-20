# 通用向量化回归（PYADAMAS_VECT）：整数数组逐元素 map（paddq/psubq 模 2^64 精确）。
# c[i]=a[i]+b[i] / a[i]-b[i] / a[i]+不变量；n 偶/奇/1/0。整数 reduction of expr。
def addv(a: list[int], b: list[int], c: list[int], n: int) -> None:
    for i in range(n):
        c[i] = a[i] + b[i]
def subv(a: list[int], b: list[int], c: list[int], n: int) -> None:
    for i in range(n):
        c[i] = a[i] - b[i]
def addk(a: list[int], c: list[int], n: int, k: int) -> None:
    for i in range(n):
        c[i] = a[i] + k
def sumexpr(a: list[int], b: list[int], n: int) -> int:
    s = 0
    for i in range(n):
        s += a[i] + b[i]
    return s
def mk(n: int, seed: int) -> list[int]:
    a = []
    for i in range(n):
        a.append((i * seed + 3) % 101 - 50)
    return a
for n in (8, 7, 1, 0, 2):
    a = mk(n, 7); b = mk(n, 5); c = mk(n, 1)
    addv(a, b, c, n)
    s = 0
    for i in range(n):
        s += c[i]
    print("add", n, s)
    subv(a, b, c, n)
    s = 0
    for i in range(n):
        s += c[i]
    print("sub", n, s)
    addk(a, c, n, 1000)
    s = 0
    for i in range(n):
        s += c[i]
    print("addk", n, s)
    print("sumexpr", n, sumexpr(a, b, n))

# --- expect ---
# add 8 -416
# sub 8 56
# addk 8 7820
# sumexpr 8 -416
# add 7 -406
# sub 7 42
# addk 7 6818
# sumexpr 7 -406
# add 1 -94
# sub 1 0
# addk 1 953
# sumexpr 1 -94
# add 0 0
# sub 0 0
# addk 0 0
# sumexpr 0 0
# add 2 -176
# sub 2 2
# addk 2 1913
# sumexpr 2 -176
