# pyadamas: vect
# 通用向量化回归（PYADAMAS_VECT）：多 reduction 并行累加器（0.351 块3）。
# 双 float / 双 int / 混合 / 共享 expr（CSE）/ mul+add / 三累加器 / 前值非0 /
# map+reduction；跨累加器依赖（s2+=s1）负例必须保守跳过且输出正确。
# 期望值由 CPython 参考生成（float 保序逐位一致，int mod 2^64）。
def two_f(a: list[float], b: list[float], out: list[float], n: int) -> None:
    s1 = 0.0
    s2 = 0.0
    for j in range(n):
        s1 += a[j]
        s2 += b[j]
    out[0] = s1
    out[1] = s2
def two_i(a: list[int], b: list[int], out: list[int], n: int) -> None:
    s1 = 0
    s2 = 0
    for j in range(n):
        s1 += a[j]
        s2 += b[j]
    out[0] = s1
    out[1] = s2
def mixed(a: list[float], b: list[int], out: list[float], n: int) -> None:
    s1 = 0.0
    s2 = 0
    for j in range(n):
        s1 += a[j]
        s2 += b[j]
    out[0] = s1
    out[1] = float(s2)
def shared(a: list[float], out: list[float], n: int) -> None:
    s1 = 0.0
    s2 = 0.0
    for j in range(n):
        s1 += a[j]
        s2 += a[j]
    out[0] = s1
    out[1] = s2
def muladd(a: list[int], b: list[int], out: list[int], n: int) -> None:
    s1 = 0
    s2 = 0
    for j in range(n):
        s1 += a[j] * b[j]
        s2 += a[j] + b[j]
    out[0] = s1
    out[1] = s2
def three_f(a: list[float], b: list[float], out: list[float], n: int) -> None:
    s1 = 0.0
    s2 = 0.0
    s3 = 0.0
    for j in range(n):
        s1 += a[j]
        s2 += b[j]
        s3 += a[j] * b[j]
    out[0] = s1
    out[1] = s2
    out[2] = s3
def prefix_f(a: list[float], b: list[float], out: list[float], n: int) -> None:
    s1 = 1.5
    s2 = 2.5
    for j in range(n):
        s1 += a[j]
        s2 += b[j]
    out[0] = s1
    out[1] = s2
def mapred(a: list[float], b: list[float], out: list[float], n: int) -> None:
    s = 0.0
    for j in range(n):
        out[j] = a[j] * 1.5
        s += b[j]
    out[n] = s
def dep_f(a: list[float], out: list[float], n: int) -> None:
    s1 = 0.0
    s2 = 0.0
    for j in range(n):
        s1 += a[j]
        s2 += s1
    out[0] = s1
    out[1] = s2
def dep_i(a: list[int], out: list[int], n: int) -> None:
    s1 = 0
    s2 = 0
    for j in range(n):
        s1 += a[j]
        s2 += s1
    out[0] = s1
    out[1] = s2
def mkf(n: int) -> list[float]:
    a = []
    for j in range(n):
        a.append(float(j) + 0.5)
    return a
def mkf2(n: int) -> list[float]:
    b = []
    for j in range(n):
        b.append(float(j) * 0.25 + 0.125)
    return b
def mki(n: int) -> list[int]:
    a = []
    for j in range(n):
        a.append(j * 4611686018427387904 - 7)
    return a
def mki2(n: int) -> list[int]:
    b = []
    for j in range(n):
        b.append(j * 4294967297 + 3)
    return b
def zf(n: int) -> list[float]:
    o = []
    for j in range(n):
        o.append(0.0)
    return o
def zi(n: int) -> list[int]:
    o = []
    for j in range(n):
        o.append(0)
    return o
for n in (8, 7, 1, 0, 2, 3):
    o = zf(3)
    two_f(mkf(n), mkf2(n), o, n)
    print("two_f", n, o[0], o[1])
    o2 = zi(2)
    two_i(mki(n), mki2(n), o2, n)
    print("two_i", n, o2[0], o2[1])
    o = zf(2)
    mixed(mkf(n), mki(n), o, n)
    print("mixed", n, o[0], o[1])
    o = zf(2)
    shared(mkf(n), o, n)
    print("shared", n, o[0], o[1])
    o2 = zi(2)
    muladd(mki(n), mki2(n), o2, n)
    print("muladd", n, o2[0], o2[1])
    o = zf(3)
    three_f(mkf(n), mkf2(n), o, n)
    print("three_f", n, o[0], o[1], o[2])
    o = zf(2)
    prefix_f(mkf(n), mkf2(n), o, n)
    print("prefix_f", n, o[0], o[1])
    o = zf(n + 1)
    mapred(mkf(n), mkf2(n), o, n)
    print("mapred", n, o[n], o[0] if n > 0 else 0.0)
    o = zf(2)
    dep_f(mkf(n), o, n)
    print("dep_f", n, o[0], o[1])
    o2 = zi(2)
    dep_i(mki(n), o2, n)
    print("dep_i", n, o2[0], o2[1])

# --- expect ---
# two_f 8 32.0 8.0
# two_i 8 -56 120259084340
# mixed 8 32.0 -56.0
# shared 8 32.0 32.0
# muladd 8 -841813590380 120259084284
# three_f 8 32.0 8.0 42.5
# prefix_f 8 33.5 10.5
# mapred 8 8.0 0.75
# dep_f 8 32.0 102.0
# dep_i 8 -56 -252
# two_f 7 24.5 6.125
# two_i 7 4611686018427387855 90194313258
# mixed 7 24.5 4.611686018427388e+18
# shared 7 24.5 24.5
# muladd 7 9223371405494583002 4611686108621701113
# three_f 7 24.5 6.125 28.4375
# prefix_f 7 26.0 8.625
# mapred 7 6.125 0.75
# dep_f 7 24.5 70.0
# dep_i 7 4611686018427387855 -196
# two_f 1 0.5 0.125
# two_i 1 -7 3
# mixed 1 0.5 -7.0
# shared 1 0.5 0.5
# muladd 1 -21 -4
# three_f 1 0.5 0.125 0.0625
# prefix_f 1 2.0 2.625
# mapred 1 0.125 0.75
# dep_f 1 0.5 0.5
# dep_i 1 -7 -7
# two_f 0 0.0 0.0
# two_i 0 0 0
# mixed 0 0.0 0.0
# shared 0 0.0 0.0
# muladd 0 0 0
# three_f 0 0.0 0.0 0.0
# prefix_f 0 1.5 2.5
# mapred 0 0.0 0.0
# dep_f 0 0.0 0.0
# dep_i 0 0 0
# two_f 2 2.0 0.5
# two_i 2 4611686018427387890 4294967303
# mixed 2 2.0 4.611686018427388e+18
# shared 2 2.0 2.0
# muladd 2 -30064771121 4611686022722355193
# three_f 2 2.0 0.5 0.625
# prefix_f 2 3.5 3.0
# mapred 2 0.5 0.75
# dep_f 2 2.0 2.5
# dep_i 2 4611686018427387890 4611686018427387883
# two_f 3 4.5 1.125
# two_i 3 -4611686018427387925 12884901900
# mixed 3 4.5 -4.611686018427388e+18
# shared 3 4.5 4.5
# muladd 3 9223371946660462508 -4611686005542486025
# three_f 3 4.5 1.125 2.1875
# prefix_f 3 6.0 3.625
# mapred 3 1.125 0.75
# dep_f 3 4.5 7.0
# dep_i 3 -4611686018427387925 -42
