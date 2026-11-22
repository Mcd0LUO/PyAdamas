# SIMD 向量化（通用 VECT）：map、reduce、int map、int mul、convert
# 0.382 合并自 5 个文件：344_vect_map.py, 345_vect_reduce.py, 349_vect_int_map.py, 375_vect_int_mul.py, 376_vect_convert.py
# 每段包在独立函数里，避免同名变量/类跨段冲突。

# ── 原 344_vect_map.py ──
def _case_1() -> None:
    # 通用循环向量化回归（PYADAMAS_VECT）：非基准浮点数组逐元素 map。
    # 任意形状 out[i]=a[i]*c+b[i]（加/乘/不变量），n 覆盖偶/奇/1/0，尾循环。
    # flag-on/off 输出相同且对拍 CPython（向量化路径见 PYADAMAS_VECT_DBG）。
    def scale_add(a: list[float], b: list[float], out: list[float], n: int, c: float) -> None:
        for i in range(n):
            out[i] = a[i] * c + b[i]

    def run(n: int) -> None:
        a = []
        b = []
        out = []
        for i in range(n):
            a.append(float(i) + 0.5)
            b.append(float(i) * 0.25)
            out.append(0.0)
        scale_add(a, b, out, n, 1.5)
        s = 0.0
        for i in range(n):
            s += out[i]
        print(n, s)

    run(8)
    run(7)
    run(1)
    run(0)
    run(2)
    run(3)

# ── 原 345_vect_reduce.py ──
def _case_2() -> None:
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

# ── 原 349_vect_int_map.py ──
def _case_3() -> None:
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

# ── 原 375_vect_int_mul.py ──
def _case_4() -> None:
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

# ── 原 376_vect_convert.py ──
def _case_5() -> None:
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

_case_1()
_case_2()
_case_3()
_case_4()
_case_5()

# --- expect ---
# 8 55.0
# 7 42.0
# 1 0.75
# 0 0.0
# 2 3.25
# 3 7.5
# 10 6.450000000000001 3.2250000000000005
# 9 5.010000000000001 2.5050000000000003
# 1 0.09 0.045
# 0 0.0 0.0
# 2 0.25 0.125
# 559 422 -7 0
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
