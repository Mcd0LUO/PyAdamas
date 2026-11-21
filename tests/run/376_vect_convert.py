# 通用向量化回归（PYADAMAS_VECT）：Convert int→float（cvtdq2pd 折算序列，
# pshufd 聚 hi/lo dword，与标量 cvtsi2sdq 逐位一致，任意 i64 含 INT64_MIN/MAX、
# >2^53 舍入、负数）。map / float(j) / float(j+2) / float(不变量) / 乘后折算 /
# float reduction（保序）。期望值由 CPython 参考生成（float() 同为 RN 舍入）。
def cv(a: list[int], o: list[float], n: int) -> None:
    for j in range(n):
        o[j] = float(a[j])
def cvj(o: list[float], n: int) -> None:
    for j in range(n):
        o[j] = float(j)
def cvj2(o: list[float], n: int) -> None:
    for j in range(n):
        o[j] = float(j + 2)
def cvk(o: list[float], n: int, k: int) -> None:
    for j in range(n):
        o[j] = float(k)
def cvexpr(a: list[int], o: list[float], n: int) -> None:
    for j in range(n):
        o[j] = float(a[j]) * 1.5 + 0.25
def cvmul(a: list[int], b: list[int], o: list[float], n: int) -> None:
    for j in range(n):
        o[j] = float(a[j] * b[j])
def cvred(a: list[int], n: int) -> float:
    s = 0.5
    for j in range(n):
        s += float(a[j])
    return s
def cvredmul(a: list[int], b: list[int], n: int) -> float:
    s = 0.0
    for j in range(n):
        s += float(a[j] * b[j])
    return s
def mk() -> list[int]:
    a = []
    a.append(9223372036854775807)
    a.append(-9223372036854775807 - 1)
    a.append(9007199254740993)
    a.append(-9007199254740993)
    a.append(2147483648)
    a.append(-2147483649)
    a.append(4294967297)
    a.append(3)
    return a
def mk2() -> list[int]:
    b = []
    b.append(3)
    b.append(-5)
    b.append(4294967296)
    b.append(2147483647)
    b.append(-4611686018427387904)
    b.append(7)
    b.append(2147483648)
    b.append(-1)
    return b
def zero(n: int) -> list[float]:
    o = []
    for j in range(n):
        o.append(0.0)
    return o
def fsum(o: list[float], n: int) -> float:
    s = 0.0
    for j in range(n):
        s += o[j]
    return s
for n in (8, 7, 1, 0, 2, 3):
    o = zero(n)
    cv(mk(), o, n)
    print("cv", n, fsum(o, n), o[0] if n > 0 else 0.0, o[n - 1] if n > 0 else 0.0)
    o = zero(n)
    cvj(o, n)
    print("cvj", n, fsum(o, n))
    o = zero(n)
    cvj2(o, n)
    print("cvj2", n, fsum(o, n))
    o = zero(n)
    cvk(o, n, 9007199254740993)
    print("cvk", n, fsum(o, n))
    o = zero(n)
    cvexpr(mk(), o, n)
    print("cvexpr", n, fsum(o, n))
    o = zero(n)
    cvmul(mk(), mk2(), o, n)
    print("cvmul", n, fsum(o, n))
    print("cvred", n, cvred(mk(), n))
    print("cvredmul", n, cvredmul(mk(), mk2(), n))

# --- expect ---
# cv 8 4294967299.0 9.223372036854776e+18 3.0
# cvj 8 28.0
# cvj2 8 44.0
# cvk 8 7.205759403792794e+16
# cvexpr 8 6442450949.5
# cvmul 8 -9.214364848337453e+18
# cvred 8 4294967299.0
# cvredmul 8 -9.214364848337453e+18
# cv 7 4294967296.0 9.223372036854776e+18 4294967297.0
# cvj 7 21.0
# cvj2 7 35.0
# cvk 7 6.305039478318694e+16
# cvexpr 7 6442450944.75
# cvmul 7 -9.214364848337453e+18
# cvred 7 4294967296.0
# cvredmul 7 -9.214364848337453e+18
# cv 1 9.223372036854776e+18 9.223372036854776e+18 9.223372036854776e+18
# cvj 1 0.0
# cvj2 1 2.0
# cvk 1 9007199254740992.0
# cvexpr 1 1.3835058055282164e+19
# cvmul 1 9.223372036854776e+18
# cvred 1 9.223372036854776e+18
# cvredmul 1 9.223372036854776e+18
# cv 0 0.0 0.0 0.0
# cvj 0 0.0
# cvj2 0 0.0
# cvk 0 0.0
# cvexpr 0 0.0
# cvmul 0 0.0
# cvred 0 0.5
# cvredmul 0 0.0
# cv 2 0.0 9.223372036854776e+18 -9.223372036854776e+18
# cvj 2 1.0
# cvj2 2 5.0
# cvk 2 1.8014398509481984e+16
# cvexpr 2 0.0
# cvmul 2 0.0
# cvred 2 0.0
# cvredmul 2 0.0
# cv 3 9007199254740992.0 9.223372036854776e+18 9007199254740992.0
# cvj 3 3.0
# cvj2 3 9.0
# cvk 3 2.7021597764222976e+16
# cvexpr 3 1.3510798882111488e+16
# cvmul 3 4294967296.0
# cvred 3 9007199254740992.0
# cvredmul 3 4294967296.0
