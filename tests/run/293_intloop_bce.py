# 整数循环 BCE 回归（0.335，PYADAMAS_LOOP_OPT）：仿射 IV 下标克隆循环。
# matmul 形态（idx = 不变偏移 + IV）+ 纯 IV 下标 + 副本槽链；多种 N（含奇数、1、0）。
# 守卫通过走 unchecked 克隆；输出对拍 CPython。
def mm(a: list[int], b: list[int], c: list[int], n: int) -> None:
    for i in range(n):
        for k in range(n):
            aik = a[i * n + k]
            if aik != 0:
                for j in range(n):
                    c[i * n + j] += aik * b[k * n + j]

def mk(n: int, seed: int) -> list[int]:
    out = []
    for i in range(n * n):
        out.append((i * seed + 3) % 11)
    return out

def total(c: list[int]) -> int:
    t = 0
    for v in c:
        t += v
    return t

for n in (7, 4, 1, 0, 3):
    a = mk(n, 7)
    b = mk(n, 5)
    c = []
    for _ in range(n * n):
        c.append(0)
    mm(a, b, c, n)
    print(n, total(c))

# 纯 IV 下标 + 副本槽：s[i] = s[i] * 2 + i（存储与读取同址）
def dbl(s: list[int], n: int) -> None:
    for i in range(n):
        s[i] = s[i] * 2 + i

s = []
for i in range(9):
    s.append(i)
dbl(s, 9)
print(s)

# 偏移是运行时不变量（非常量）：base = off + j
def shift(a: list[int], off: int, n: int) -> int:
    t = 0
    for j in range(n):
        t += a[off + j]
    return t

a = []
for i in range(20):
    a.append(i * i)
print(shift(a, 5, 10))
print(shift(a, 0, 20))

# --- expect ---
# 7 8694
# 4 1584
# 1 9
# 0 0
# 3 681
# [0, 3, 6, 9, 12, 15, 18, 21, 24]
# 985
# 2470
