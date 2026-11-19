"""d3_integrate -- 数值算子型：数值积分与收敛。

对 sin/exp/多项式在给定区间用梯形与 Simpson 法则积分，
列出 n 递增的收敛表并与解析值比较误差，Richardson 外推一次。
手动累加保证两边同序 IEEE。
"""
import math


def f_sin(x: float) -> float:
    return math.sin(x)


def f_exp(x: float) -> float:
    return math.exp(-x * x)


def f_poly(x: float) -> float:
    return x * x * x - 2.0 * x * x + 3.0 * x - 1.0


def trapezoid(a: float, b: float, n: int, which: int) -> float:
    h: float = (b - a) / float(n)
    s: float = 0.0
    for i in range(n + 1):
        x: float = a + float(i) * h
        fx: float = 0.0
        if which == 0:
            fx = f_sin(x)
        elif which == 1:
            fx = f_exp(x)
        else:
            fx = f_poly(x)
        if i == 0 or i == n:
            s += fx
        else:
            s += 2.0 * fx
    return s * h / 2.0


def simpson(a: float, b: float, n: int, which: int) -> float:
    if n % 2 == 1:
        n += 1
    h: float = (b - a) / float(n)
    s: float = 0.0
    for i in range(n + 1):
        x: float = a + float(i) * h
        fx: float = 0.0
        if which == 0:
            fx = f_sin(x)
        elif which == 1:
            fx = f_exp(x)
        else:
            fx = f_poly(x)
        if i == 0 or i == n:
            s += fx
        elif i % 2 == 1:
            s += 4.0 * fx
        else:
            s += 2.0 * fx
    return s * h / 3.0


def main() -> None:
    print("== integral sin(x) on [0, pi] (exact 2) ==")
    exact_sin: float = 2.0
    for n in [10, 100, 1000, 10000]:
        t: float = trapezoid(0.0, math.pi, n, 0)
        s: float = simpson(0.0, math.pi, n, 0)
        print(n, "trap", format(t, ".12f"), "err", format(abs(t - exact_sin), ".3e"),
              "simp", format(s, ".12f"), "err", format(abs(s - exact_sin), ".3e"))
    print("== integral exp(-x^2) on [0, 1] ==")
    ref: float = simpson(0.0, 1.0, 20000, 1)
    for n in [10, 100, 1000]:
        s: float = simpson(0.0, 1.0, n, 1)
        print(n, "simp", format(s, ".12f"), "diff_ref", format(abs(s - ref), ".3e"))
    print("== integral poly on [0, 2] (exact 6) ==")
    exact_poly: float = 2.0 ** 4 / 4.0 - 2.0 * 2.0 ** 3 / 3.0 + 3.0 * 2.0 ** 2 / 2.0 - 2.0
    for n in [10, 100, 1000]:
        s: float = simpson(0.0, 2.0, n, 2)
        print(n, "simp", format(s, ".12f"), "err", format(abs(s - exact_poly), ".3e"))
    print("== Richardson extrapolation (sin) ==")
    s1: float = simpson(0.0, math.pi, 100, 0)
    s2: float = simpson(0.0, math.pi, 200, 0)
    rich: float = s2 + (s2 - s1) / 15.0
    print("richardson", format(rich, ".14f"), "err", format(abs(rich - exact_sin), ".3e"))


if __name__ == "__main__":
    main()
