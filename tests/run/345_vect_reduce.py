# 通用循环向量化回归（PYADAMAS_VECT）：非基准浮点 reduction（保序累加，逐位一致）。
# dot / 平方和 / 带不变量系数，n 偶/奇/1/0；整数 reduction（paddq 精确）。
def dot(a: list[float], b: list[float], n: int) -> float:
    s = 0.0
    for i in range(n):
        s += a[i] * b[i]
    return s

def sqsum(a: list[float], n: int, k: float) -> float:
    s = 0.0
    for i in range(n):
        s += a[i] * a[i] * k
    return s

def isum(a: list[int], n: int) -> int:
    s = 0
    for i in range(n):
        s += a[i]
    return s

def mkf(n: int) -> list[float]:
    a = []
    for i in range(n):
        a.append(float(i) * 0.1 + 0.3)
    return a

for n in (10, 9, 1, 0, 2):
    a = mkf(n)
    b = mkf(n)
    print(n, dot(a, b, n), sqsum(a, n, 0.5))
ia = []
for i in range(13):
    ia.append(i * i - 7)
print(isum(ia, 13), isum(ia, 12), isum(ia, 1), isum(ia, 0))

# --- expect ---
# 10 6.450000000000001 3.2250000000000005
# 9 5.010000000000001 2.5050000000000003
# 1 0.09 0.045
# 0 0.0 0.0
# 2 0.25 0.125
# 559 422 -7 0
