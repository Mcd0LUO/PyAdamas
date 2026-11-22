# pyadamas: simd
# SIMD reduction/map 综合：reduction 与标量 fallback、int map 与标量 fallback
# 0.383 合并自 4 个文件：283_simd_reduction.py, 284_simd_reduction_scalar.py, 285_simd_int_map.py, 286_simd_int_map_scalar.py
# 每段包在独立函数里，避免同名变量跨段冲突。

# ── 原 283_simd_reduction.py ──
def _case_1() -> None:
    # SIMD reduction 向量化回归（0.333，PYADAMAS_SIMD）：spectral_norm 形态 N=5（奇数）
    # 钉住 eval_A 乘加累加 reduction 输出。保序累加 → 与标量逐位一致，flag-on/off
    # 输出相同且对拍 CPython。mul_ata(a_is_j=0)+mul_a(a_is_j=1) 两条向量路径 + 奇数尾循环。
    N = 5
    def eval_a(i: int, j: int) -> float:
        return 1.0 / (((i + j) * (i + j + 1) // 2) + i + 1)
    def mul_ata(v: list[float], out: list[float], n: int) -> None:
        for i in range(n):
            total = 0.0
            for j in range(n):
                total += eval_a(i, j) * v[j]
            out[i] = total
    def mul_a(u: list[float], out: list[float], n: int) -> None:
        for i in range(n):
            total = 0.0
            for j in range(n):
                total += eval_a(j, i) * u[j]
            out[i] = total
    u = []
    v = []
    tmp = []
    for _ in range(N):
        u.append(1.0)
        v.append(0.0)
        tmp.append(0.0)
    for _ in range(10):
        mul_ata(u, v, N)
        mul_a(v, tmp, N)
    v_bv = 0.0
    v_vv = 0.0
    for i in range(N):
        v_bv += u[i] * tmp[i]
        v_vv += tmp[i] * tmp[i]
    print(int((v_bv / v_vv) ** 0.5 * 1000000.0))

# ── 原 284_simd_reduction_scalar.py ──
def _case_2() -> None:
    # SIMD reduction 标量正确性回归（0.333）：覆盖向量化守卫的 fallback 路径与
    # 边界 n。这些形态走标量（内循环常量未外提 / 大范围守卫失败 / len<n），
    # 输出钉住对拍 CPython，确保 fallback 与异常语义不变。


    def eval_a(i: int, j: int) -> float:
        return 1.0 / (((i + j) * (i + j + 1) // 2) + i + 1)


    def spec_scalar(n: int) -> int:
        a = []
        b = []
        t = []
        for _ in range(n):
            a.append(1.0)
            b.append(0.0)
            t.append(0.0)
        for _ in range(10):
            for i in range(n):
                s = 0.0
                for j in range(n):
                    s += eval_a(i, j) * a[j]
                b[i] = s
            for i in range(n):
                s = 0.0
                for j in range(n):
                    s += eval_a(j, i) * b[j]
                t[i] = s
        bv = 0.0
        vv = 0.0
        for i in range(n):
            bv += a[i] * t[i]
            vv += t[i] * t[i]
        return 0 if vv == 0.0 else int((bv / vv) ** 0.5 * 1000000.0)


    # n<=1（向量守卫 j+1<n 失败 → 标量）、偶数、奇数。
    print("n1", spec_scalar(1))
    print("n2", spec_scalar(2))
    print("n3", spec_scalar(3))
    print("n8", spec_scalar(8))
    print("n17", spec_scalar(17))


    # 大范围 fallback：i+n > 46341 时范围守卫失败走标量（结果仍正确）。
    def row_sum(v: list[float], i: int, n: int) -> float:
        total = 0.0
        for j in range(n):
            total += eval_a(i, j) * v[j]
        return total


    vv = [1.0] * 12
    print("small", format(row_sum(vv, 0, 12), ".12f"))
    print("big", format(row_sum(vv, 50000, 12), ".12f"))


    # len < n：IndexError 语义与时机保留。
    def mul_ata_chk(v: list[float], out: list[float], n: int) -> None:
        for i in range(n):
            total = 0.0
            for j in range(n):
                total += eval_a(i, j) * v[j]
            out[i] = total


    short = [1.0, 2.0]
    try:
        mul_ata_chk(short, [0.0] * 5, 5)
        print("no error")
    except IndexError:
        print("IndexError ok")

# ── 原 285_simd_int_map.py ──
def _case_3() -> None:
    # SIMD int map 向量化回归（0.334，PYADAMAS_SIMD）：matmul 形态 c[base+j] += aik*b[base2+j]。
    # 守卫范围内（aik∈[0,2^31)、b/c 非负小值）走向量路径：N=7 奇数尾 + N=4 偶数吃尽 + N=1。
    # pmuludq 组合乘 + paddq 与标量 imulq/addq 逐位一致；输出对拍 CPython。
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

    for n in (7, 4, 1):
        a = mk(n, 7)
        b = mk(n, 5)
        c = []
        for _ in range(n * n):
            c.append(0)
        mm(a, b, c, n)
        print(n, total(c))

# ── 原 286_simd_int_map_scalar.py ──
def _case_4() -> None:
    # SIMD int map 标量 fallback / 边界回归（0.334）：守卫不满足必须整段走标量，
    # 语义与时机不变。期望输出是 pyadamas i64 回绕语义（flag-on==off==O0==O1）；
    # 溢出/大数 case 与 CPython 任意精度不同（设计口径，见 docs §10），其余对拍 CPython。
    def mm(a: list[int], b: list[int], c: list[int], n: int) -> None:
        for i in range(n):
            for k in range(n):
                aik = a[i * n + k]
                if aik != 0:
                    for j in range(n):
                        c[i * n + j] += aik * b[k * n + j]

    def mmu(a, b, c, n: int) -> None:
        for i in range(n):
            for k in range(n):
                aik = a[i * n + k]
                if aik != 0:
                    for j in range(n):
                        c[i * n + j] += aik * b[k * n + j]

    imax = (1 << 62) * 2 - 1
    imin = (1 << 62) * -2
    # 1) 负 aik → 守卫 fallback（n=5 奇数）
    a = []
    b = []
    c = []
    for i in range(25):
        a.append((i * 7 + 3) % 13 - 6)
        b.append((i * 5 + 3) % 13 - 6)
        c.append(0)
    mm(a, b, c, 5)
    t = 0
    for v in c:
        t += v
    print('neg', t)
    # 2) aik >= 2^31 → 守卫 fallback；b 大数
    a2 = [2147483648, 0, 0, 5]
    b2 = [3, 0, 0, 4294967299]
    c2 = [0, 0, 0, 0]
    mm(a2, b2, c2, 2)
    print('bigaik', c2[0], c2[3])
    # 3) b 与 c 同对象 → 别名守卫 fallback（标量顺序语义）
    a3 = [1, 1, 1, 1]
    c3 = [1, 2, 3, 4]
    mm(a3, c3, c3, 2)
    print('alias', c3[0], c3[1], c3[2], c3[3])
    # 4) aik 小正数 + b 超 2^32/负数 → 向量路径 pmuludq 组合精确重构（回绕）
    a4 = [1, 2, 0, 3]
    b4 = [4294967296, -4294967297, imax, imin]
    c4 = [0, 0, 0, 0]
    mm(a4, b4, c4, 2)
    print('bigb', c4[0], c4[1], c4[2], c4[3])
    # 5) 累加越过 2^63 → paddq 回绕 == addq
    a5 = [(1 << 62), 0, 0, (1 << 62)]
    b5 = [2, 0, 0, 3]
    c5 = [imax - 7, 0, 0, 0]
    mm(a5, b5, c5, 2)
    print('wrap', c5[0], c5[3])
    # 6) IndexError 时机：c 短 / b 短，第一个越界 j 处抛
    a6 = [1, 1, 1, 1]
    b6 = [1, 1, 1, 1]
    c6 = [0, 0, 0]
    try:
        mm(a6, b6, c6, 2)
    except IndexError:
        print('idxc', c6[0], c6[1], c6[2])
    c7 = [0, 0, 0, 0]
    b7 = [1, 1, 1]
    try:
        mm(a6, b7, c7, 2)
    except IndexError:
        print('idxb', c7[0])
    # 7) kind 混合（c 含 float）→ kind!=0 守卫 fallback
    a8 = [1, 1, 1, 1]
    b8 = [1, 1, 1, 1]
    c8 = [0, 0, 0, 0]
    c8[1] = 0.5
    mmu(a8, b8, c8, 2)
    print('kind', c8[0], c8[1], c8[2], c8[3])
    # 8) n=0 零次循环
    c9 = [7]
    mm(a6, b6, c9, 0)
    print('zero', c9[0])

_case_1()
_case_2()
_case_3()
_case_4()

# --- expect ---
# 794176
# n1 1000000
# n2 845083
# n3 810840
# n8 790822
# n17 793975
# small 2.207752211295
# big 0.000000009597
# IndexError ok
# 7 8694
# 4 1584
# 1 9
# neg -31
# bigaik 6442450944 21474836495
# alias 5 8 16 24
# bigb 4294967294 -4294967297 9223372036854775805 -9223372036854775808
# wrap -8 -4611686018427387904
# idxc 2 2 1
# idxb 2
# kind 2.0 2.5 2.0 2.0
# zero 7
