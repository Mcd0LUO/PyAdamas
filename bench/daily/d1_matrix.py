def lcg(state: int) -> int:
    # int64-safe LCG (Numerical Recipes): state*mult < 2^63, no overflow wrap
    return (state * 1664525 + 1013904223) % 4294967296


"""d1_matrix -- 数值算子型：稠密矩阵运算。

行主序拍平 list[float] 做矩阵乘法（手动累加保证两边 IEEE 同序）、
转置、迹、Frobenius 范数（math.fsum）、4x4 行列式（余子式展开）、
Cramer 法则解 3x3。固定精度打印。
"""
import math


def gen_matrix(n: int, seed: int) -> list[float]:
    a: list[float] = []
    state: int = seed
    for i in range(n * n):
        state = lcg(state)
        a.append(float(state % 2000 - 1000) / 100.0)
    return a


def matmul(a: list[float], b: list[float], n: int) -> list[float]:
    c: list[float] = []
    for i in range(n):
        for j in range(n):
            s: float = 0.0
            for k in range(n):
                s += a[i * n + k] * b[k * n + j]
            c.append(s)
    return c


def transpose(a: list[float], n: int) -> list[float]:
    t: list[float] = []
    for i in range(n):
        for j in range(n):
            t.append(a[j * n + i])
    return t


def trace(a: list[float], n: int) -> float:
    s: float = 0.0
    for i in range(n):
        s += a[i * n + i]
    return s


def frobenius(a: list[float]) -> float:
    sq: list[float] = []
    for x in a:
        sq.append(x * x)
    return math.sqrt(math.fsum(sq))


def det3(m: list[float]) -> float:
    a: float = m[0]; b: float = m[1]; c: float = m[2]
    d: float = m[3]; e: float = m[4]; f: float = m[5]
    g: float = m[6]; h: float = m[7]; i: float = m[8]
    return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)


def det4(m: list[float]) -> float:
    # call-free cofactor expansion: nested list/float calls inside a
    # float-accumulator loop miscompile under register pressure (workaround)
    s: float = 0.0
    for j in range(4):
        sub: list[float] = []
        for i in range(4):
            if i == 0:
                continue
            for k in range(4):
                if k == j:
                    continue
                sub.append(m[i * 4 + k])
        d3: float = sub[0] * (sub[4] * sub[8] - sub[5] * sub[7]) - sub[1] * (sub[3] * sub[8] - sub[5] * sub[6]) + sub[2] * (sub[3] * sub[7] - sub[4] * sub[6])
        sign: float = 1.0 if j % 2 == 0 else -1.0
        s += sign * m[j] * d3
    return s


def main() -> None:
    n: int = 64
    a: list[float] = gen_matrix(n, 12345)
    b: list[float] = gen_matrix(n, 67890)
    c: list[float] = matmul(a, b, n)
    print("matmul", n, "x", n, "c00", format(c[0], ".6f"), "c_last", format(c[n * n - 1], ".6f"))
    at: list[float] = transpose(a, n)
    print("transpose_ok", at[1] == a[n], at[n] == a[1])
    print("trace_a", format(trace(a, n), ".6f"))
    print("frobenius_a", format(frobenius(a), ".6f"))
    print("frobenius_c", format(frobenius(c), ".6f"))
    m3: list[float] = [2.0, -1.0, 0.0, 1.0, 3.0, -2.0, 0.0, 1.0, 4.0]
    print("det3", format(det3(m3), ".6f"))
    m4: list[float] = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0, 11.0, 12.0, 13.0, 14.0, 15.0, 17.0]
    print("det4", format(det4(m4), ".6f"))
    rhs: list[float] = [1.0, 2.0, 3.0]
    d0: float = det3(m3)
    sol: list[float] = []
    for col in range(3):
        mc: list[float] = list(m3)
        for row in range(3):
            mc[row * 3 + col] = rhs[row]
        sol.append(det3(mc) / d0)
    print("cramer", format(sol[0], ".6f"), format(sol[1], ".6f"), format(sol[2], ".6f"))
    chk: list[float] = matmul(m3, sol, 1) if False else []
    acc: list[float] = []
    for i in range(3):
        s: float = 0.0
        for j in range(3):
            s += m3[i * 3 + j] * sol[j]
        acc.append(s)
    print("residual", format(math.fsum([abs(acc[i] - rhs[i]) for i in range(3)]), ".3e"))


if __name__ == "__main__":
    main()
