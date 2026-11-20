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

# --- expect ---
# n1 1000000
# n2 845083
# n3 810840
# n8 790822
# n17 793975
# small 2.207752211295
# big 0.000000009597
# IndexError ok
