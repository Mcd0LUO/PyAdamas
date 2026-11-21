# 通用向量化回归（PYADAMAS_VECT）：整数乘法 map/reduction（pmuludq 奇偶 lane
# 全 i64 重建，模 2^64 与标量 imulq 逐位一致，任意 i64 含负数/大值无需守卫）。
# c[i]=a[i]*b[i] / a[i]*k / 原地平方 / 临时槽 t*t 形状 / dot reduction。
# n 偶/奇/1/0/2/3 尾循环。期望值由 CPython 参考（显式 mod 2^64 回绕）生成。
def mulv(a: list[int], b: list[int], c: list[int], n: int) -> None:
    for i in range(n):
        c[i] = a[i] * b[i]
def mulk(a: list[int], c: list[int], n: int, k: int) -> None:
    for i in range(n):
        c[i] = a[i] * k
def square(a: list[int], n: int) -> None:
    for i in range(n):
        a[i] = a[i] * a[i]
def double_prod(a: list[int], b: list[int], c: list[int], n: int) -> None:
    for i in range(n):
        t = a[i] * b[i]
        c[i] = t + t
def dot(a: list[int], b: list[int], n: int) -> int:
    s = 0
    for i in range(n):
        s += a[i] * b[i]
    return s
def acc_prefix(n: int) -> int:
    t = 49
    for i in range(n):
        t += n
    return t
def dot_prefix(a: list[int], b: list[int], n: int) -> int:
    s = 5
    for i in range(n):
        s += a[i] * b[i]
    return s
def acc_inv(n: int, k: int) -> int:
    s = 7
    for i in range(n):
        s += k
    return s
def mk() -> list[int]:
    a = []
    a.append(4611686018427387904)
    a.append(-4611686018427387904)
    a.append(2147483647)
    a.append(2147483648)
    a.append(4294967296)
    a.append(9223372036854775807)
    a.append(-9223372036854775807 - 1)
    a.append(-3)
    return a
def mk2() -> list[int]:
    b = []
    b.append(3)
    b.append(4294967297)
    b.append(-2147483648)
    b.append(65536)
    b.append(-4611686018427387904)
    b.append(2)
    b.append(-9223372036854775807 - 1)
    b.append(4611686018427387904)
    return b
for n in (8, 7, 1, 0, 2, 3):
    a = mk(); b = mk2(); c = mk()
    mulv(a, b, c, n)
    s = 0
    for i in range(n):
        s += c[i]
    print("mulv", n, s, c[0], c[n - 1] if n > 0 else 0)
    a = mk(); c = mk()
    mulk(a, c, n, 4294967297)
    s = 0
    for i in range(n):
        s += c[i]
    print("mulk", n, s)
    a = mk()
    square(a, n)
    s = 0
    for i in range(n):
        s += a[i]
    print("square", n, s)
    a = mk(); b = mk2(); c = mk()
    double_prod(a, b, c, n)
    s = 0
    for i in range(n):
        s += c[i]
    print("double_prod", n, s)
    print("dot", n, dot(mk(), mk2(), n))

for n in (8, 7, 1, 0, 2, 3):
    print("acc_prefix", n, acc_prefix(n))
    print("dot_prefix", n, dot_prefix(mk(), mk2(), n))
    print("acc_inv", n, acc_inv(n, 4294967297))

# --- expect ---
# mulv 8 -9223231297218936834 -4611686018427387904 4611686018427387904
# mulk 8 -12884901893
# square 8 9223372032559808523
# double_prod 8 281479271677948
# dot 8 -9223231297218936834
# mulv 7 4611826758063226878 -4611686018427387904 0
# mulk 7 -2
# square 7 9223372032559808514
# double_prod 7 -9223090557583097860
# dot 7 4611826758063226878
# mulv 1 -4611686018427387904 -4611686018427387904 -4611686018427387904
# mulk 1 4611686018427387904
# square 1 0
# double_prod 1 -9223372036854775808
# dot 1 -4611686018427387904
# mulv 0 0 4611686018427387904 0
# mulk 0 0
# square 0 0
# double_prod 0 0
# dot 0 0
# mulv 2 -9223372036854775808 -4611686018427387904 -4611686018427387904
# mulk 2 0
# square 2 0
# double_prod 2 0
# dot 2 -9223372036854775808
# mulv 3 4611686020574871552 -4611686018427387904 -4611686016279904256
# mulk 3 9223372034707292159
# square 3 4611686014132420609
# double_prod 3 -9223372032559808512
# dot 3 4611686020574871552
# acc_prefix 8 113
# dot_prefix 8 -9223231297218936829
# acc_inv 8 34359738383
# acc_prefix 7 98
# dot_prefix 7 4611826758063226883
# acc_inv 7 30064771086
# acc_prefix 1 50
# dot_prefix 1 -4611686018427387899
# acc_inv 1 4294967304
# acc_prefix 0 49
# dot_prefix 0 5
# acc_inv 0 7
# acc_prefix 2 53
# dot_prefix 2 -9223372036854775803
# acc_inv 2 8589934601
# acc_prefix 3 58
# dot_prefix 3 4611686020574871557
# acc_inv 3 12884901898
