# pyadamas: vect
# 通用循环向量化回归（PYADAMAS_VECT）：P1 仿射下标 coef*j + inv + off。
# 覆盖：跨步装载（coef=2/3，含常量偏移）、运行期不变量基址（k*n+j）、跨步
# 归约（int/float）、同对象读写的 inv 判等守卫（相等 → 向量，不等 → 标量）、
# 负基址/越界守卫（保守标量，IndexError 时机不变）、n=0/1/2 边界。
def strided(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = a[2 * j] * 3

def strided_off(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = a[2 * j + 1] - 1

def row(a: list[int], b: list[int], out: list[int], n: int, k: int) -> None:
    base = k * n
    for j in range(n):
        out[j] = a[base + 2 * j] + b[base + j]

def sdot(a: list[int], n: int) -> int:
    s = 0
    for j in range(n):
        s += a[2 * j]
    return s

def fdot(a: list[float], n: int) -> float:
    s = 0.0
    for j in range(n):
        s += a[3 * j]
    return s

def self_shift(a: list[int], n: int, i: int) -> None:
    for j in range(n):
        a[j] = a[j + i] + 1

def neg_base(a: list[int], out: list[int], n: int, base: int) -> None:
    for j in range(n):
        out[j] = a[base + 2 * j]

def fmix(a: list[float], out: list[float], n: int) -> None:
    for j in range(n):
        out[j] = a[2 * j] + 0.5

def mix(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = a[j] + a[2 * j]

def two_inv(a: list[int], out: list[int], n: int, p: int, q: int) -> None:
    for j in range(n):
        out[j] = a[p + j] + a[q + 2 * j]

def red_mix(a: list[float], n: int) -> float:
    s = 0.0
    for j in range(n):
        s += a[j] * 0.5 + a[3 * j]
    return s

def zerof(n: int) -> list[float]:
    a: list[float] = []
    for i in range(n):
        a.append(0.0)
    return a

def mk(n: int) -> list[int]:
    a: list[int] = []
    for i in range(n):
        a.append((i * 7 + 3) % 11)
    return a

def mkf(n: int) -> list[float]:
    a: list[float] = []
    for i in range(n):
        a.append(float(i) + 0.5)
    return a

def zero(n: int) -> list[int]:
    a: list[int] = []
    for i in range(n):
        a.append(0)
    return a

for n in (6, 5, 1, 0, 2, 3):
    a = mk(2 * n + 3)
    out = zero(n)
    strided(a, out, n)
    print("sd", n, out)
    o2 = zero(n)
    strided_off(a, o2, n)
    print("sd0", n, o2)
    print("sdot", n, sdot(a, n))
    f = mkf(3 * n + 3)
    print("fdot", n, fdot(f, n))

for k in (0, 1, 2):
    n = 5
    a = mk(4 * n + 8)
    b = mk(4 * n + 8)
    out = zero(n)
    row(a, b, out, n, k)
    print("row", k, out)

for i in (0, 1, 2):
    a = mk(12)
    self_shift(a, 9, i)
    print("self", i, a)

for base in (0, 2, -1):
    a = mk(12)
    out = zero(4)
    try:
        neg_base(a, out, 4, base)
        print("neg", base, out)
    except IndexError:
        print("neg", base, "IndexError")

# 越界：向量守卫必须失败，按标量时机抛 IndexError
a = mk(5)
out = zero(5)
try:
    strided(a, out, 5)
    print("oob no error")
except IndexError:
    print("oob IndexError", out)


# 一个数组上混合 coef（coef==1 走 movupd、coef>1 走跨步计数器）+ 两个运行期
# 不变量基址 + 浮点跨步归约（对应 P1 的 matmul 之外的形状）
for n in (4, 5, 9):
    f = mkf(2 * n + 4)
    o = zerof(n)
    fmix(f, o, n)
    print("fmix", n, o)
    a = mk(2 * n + 4)
    oi = zero(n)
    mix(a, oi, n)
    print("mix", n, oi)
    o2 = zero(n)
    two_inv(a, o2, n, 3, 1)
    print("two", n, o2)
    print("redmix", n, red_mix(mkf(3 * n + 4), n))


NG = 4
g = mkf(2 * NG + 4)
go = zerof(NG)
fmix(g, go, NG)
print("g_fmix", go)
ga = mk(2 * NG + 4)
gio = zero(NG)
mix(ga, gio, NG)
print("g_mix", gio)
g2 = zero(NG)
two_inv(ga, g2, NG, 3, 1)
print("g_two", g2)
print("g_red", red_mix(mkf(3 * NG + 4), NG))

# --- expect ---
# sd 6 [9, 18, 27, 3, 12, 21]
# sd0 6 [9, 1, 4, 7, -1, 2]
# sdot 6 30
# fdot 6 48.0
# sd 5 [9, 18, 27, 3, 12]
# sd0 5 [9, 1, 4, 7, -1]
# sdot 5 23
# fdot 5 32.5
# sd 1 [9]
# sd0 1 [9]
# sdot 1 3
# fdot 1 0.5
# sd 0 []
# sd0 0 []
# sdot 0 0
# fdot 0 0.0
# sd 2 [9, 18]
# sd0 2 [9, 1]
# sdot 2 9
# fdot 2 4.0
# sd 3 [9, 18, 27]
# sd0 3 [9, 1, 4]
# sdot 3 18
# fdot 3 10.5
# row 0 [6, 16, 15, 3, 13]
# row 1 [10, 9, 8, 7, 6]
# row 2 [14, 13, 12, 11, 10]
# self 0 [4, 11, 7, 3, 10, 6, 2, 9, 5, 0, 7, 3]
# self 1 [11, 7, 3, 10, 6, 2, 9, 5, 1, 0, 7, 3]
# self 2 [7, 3, 10, 6, 2, 9, 5, 1, 8, 0, 7, 3]
# neg 0 [3, 6, 9, 1]
# neg 2 [6, 9, 1, 4]
# neg -1 [3, 10, 2, 5]
# oob IndexError [9, 18, 27, 0, 0]
# fmix 4 [1.0, 3.0, 5.0, 7.0]
# mix 4 [6, 16, 15, 3]
# two 4 [12, 11, 10, 9]
# redmix 4 24.0
# fmix 5 [1.0, 3.0, 5.0, 7.0, 9.0]
# mix 5 [6, 16, 15, 3, 13]
# two 5 [12, 11, 10, 9, 8]
# redmix 5 38.75
# fmix 9 [1.0, 3.0, 5.0, 7.0, 9.0, 11.0, 13.0, 15.0, 17.0]
# mix 9 [6, 16, 15, 3, 13, 12, 11, 10, 9]
# two 9 [12, 11, 10, 9, 8, 7, 6, 16, 4]
# redmix 9 132.75
# g_fmix [1.0, 3.0, 5.0, 7.0]
# g_mix [6, 16, 15, 3]
# g_two [12, 11, 10, 9]
# g_red 24.0
