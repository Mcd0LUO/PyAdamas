# pyadamas: vect
# 通用向量化深化负例（PYADAMAS_VECT，0.351 块4）：int 乘 / Convert / 多
# reduction 新路径的别名/依赖证不出 → 保守标量 fallback，输出仍须 ==CPython。
# 覆盖：跨迭代依赖、运行时同对象别名、写后读、写偏移非0、循环变量外读、
# len<n 的 IndexError 时机。小值数据（无回绕）→ CPython 原样对拍。
def mulv(a: list[int], b: list[int], c: list[int], n: int) -> None:
    for i in range(n):
        c[i] = a[i] * b[i]
def dot(a: list[int], b: list[int], n: int) -> int:
    s = 0
    for i in range(n):
        s += a[i] * b[i]
    return s
def mul_dep(a: list[int], n: int) -> None:
    for i in range(n):
        a[i] = a[i] * a[i + 1]
def mul_ip(a: list[int], b: list[int], n: int) -> None:
    for i in range(n):
        a[i] = a[i] * b[i]
def mul_off(a: list[int], b: list[int], out: list[int], n: int) -> None:
    for i in range(n):
        out[i + 1] = a[i] * b[i]
def cv(a: list[int], o: list[float], n: int) -> None:
    for j in range(n):
        o[j] = float(a[j])
def cv_wr(a: list[int], out: list[float], n: int) -> float:
    s = 0.0
    for i in range(n):
        out[i] = float(a[i])
        s += out[i]
    return s
def cvmul(a: list[int], b: list[int], o: list[float], n: int) -> None:
    for j in range(n):
        o[j] = float(a[j] * b[j])
def cv_live(o: list[float], n: int) -> None:
    for i in range(n):
        o[i] = float(i)
    print("cvlast", i)
def mr_wr(a: list[int], b: list[int], out: list[int], n: int) -> int:
    s = 0
    for i in range(n):
        out[i] = a[i] + b[i]
        s += out[i]
    return s
def mk(n: int) -> list[int]:
    a = []
    for i in range(n):
        a.append(i * 3 - 7)
    return a
def mk2(n: int) -> list[int]:
    b = []
    for i in range(n):
        b.append(i * 5 + 2)
    return b
def mf(n: int) -> list[float]:
    o = []
    for i in range(n):
        o.append(0.0)
    return o
# 运行时别名：mulv(a, a, c) / dot(a, a) / mul_ip(a, a) / cvmul(a, a, o)
a = mk(6)
c = mk(6)
mulv(a, a, c, 6)
print("alias_mul", c)
print("alias_dot", dot(a, a, 6))
b = mk(6)
mul_ip(b, b, 6)
print("alias_ip", b)
o = mf(6)
cvmul(a, a, o, 6)
print("alias_cv", o)
# 跨迭代依赖（读 off1 写 off0 同数组）
d = mk(6)
mul_dep(d, 5)
print("mul_dep", d)
# 写偏移非0
e = mk(6)
f = mk2(6)
g = mk(7)
mul_off(e, f, g, 6)
print("mul_off", g)
# Convert 写后读
h = mk(6)
o2 = mf(6)
print("cv_wr", cv_wr(h, o2, 6), o2)
# 多 reduction 写后读
print("mr_wr", mr_wr(mk(6), mk2(6), mk(6), 6))
# 循环变量外读（Convert 路径）
o3 = mf(4)
cv_live(o3, 4)
print("cv_live", o3)
# IndexError 时机：len<n，第一个越界 j 抛（守卫 len>=n 失败整段标量）
try:
    mulv(mk(3), mk2(3), mk(3), 5)
    print("no error")
except IndexError:
    print("mul IndexError")
try:
    cv(mk(3), mf(3), 5)
    print("no error")
except IndexError:
    print("cv IndexError")
# n=0/1 边界（乘/折算）
c1 = mk(1)
mulv(mk(1), mk2(1), c1, 1)
print("n1", c1)
o1 = mf(1)
cv(mk(1), o1, 1)
print("cvn1", o1)
print("n0", dot(mk(0), mk2(0), 0))

# --- expect ---
# alias_mul [49, 16, 1, 4, 25, 64]
# alias_dot 159
# alias_ip [49, 16, 1, 4, 25, 64]
# alias_cv [49.0, 16.0, 1.0, 4.0, 25.0, 64.0]
# mul_dep [28, 4, -2, 10, 40, 8]
# mul_off [-7, -14, -28, -12, 34, 110, 216]
# cv_wr 3.0 [-7.0, -4.0, -1.0, 2.0, 5.0, 8.0]
# mr_wr 90
# cvlast 3
# cv_live [0.0, 1.0, 2.0, 3.0]
# mul IndexError
# cv IndexError
# n1 [-14]
# cvn1 [-7.0]
# n0 0
