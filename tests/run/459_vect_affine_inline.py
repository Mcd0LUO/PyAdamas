# pyadamas: vect
# 通用循环向量化回归（PYADAMAS_VECT）：仿射下标 + 内联后的跨块 mov 链。
# 这段形状会触发「数组基址经跨块 mov 解析到源 vreg 的 home」—— 源的活跃区间
# 已在循环外结束（内联 RC move-from 清槽后栈槽被复用），取源槽会在向量前奏
# 里读到别的值（实测段错误）。基址必须取 mov 自己的 home。
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
def mk(n: int) -> list[int]:
    a: list[int] = []
    for i in range(n):
        a.append((i * 5 + 1) % 13)
    return a
def mkf(n: int) -> list[float]:
    a: list[float] = []
    for i in range(n):
        a.append(float(i) * 0.25)
    return a
def zero(n: int) -> list[int]:
    a: list[int] = []
    for i in range(n):
        a.append(0)
    return a
def zerof(n: int) -> list[float]:
    a: list[float] = []
    for i in range(n):
        a.append(0.0)
    return a
n = 4
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

# --- expect ---
# fmix 4 [0.5, 1.0, 1.5, 2.0]
# mix 4 [2, 17, 19, 8]
# two 4 [9, 11, 0, 15]
# redmix 4 5.25
