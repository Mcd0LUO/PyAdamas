# pyadamas: vect
# float 减号归约（s = s - expr）的 2-lane 向量化回归（阶段 B3）：
#   * 归约识别原来只认 BinKind::Add；本轮放行 float 的 Sub（累加器在左），
#     发射期按 lane 顺序发 subsd（先 lane0 再 lane1）→ 与标量逐位一致；
#   * int 的 Sub 归约仍保守走标量（paddq 那条线只覆盖加，neg 要额外 scratch）；
#   * sqrt + 减号归约组合（nbody energy 内层的形状）现在也能向量化；
#   * 覆盖 n=0/1/2/3/5/7（含奇数尾）、负数、共享表达式（FSCR 副本提取）、
#     混合量级（舍入顺序必须与标量一致）。
import math


def _case_sub(n: int) -> None:
    a = [0.25, 1.0, -2.25, 4.0, -6.25, 9.0, 12.25]
    s = 0.0
    for j in range(n):
        s = s - a[j]
    print(n, s)


def _case_sub_neg() -> None:
    a = [-0.5, 2.5, -3.5, 4.5, -5.5]
    s = 1.5
    for j in range(5):
        s = s - a[j]
    print(s)


def _case_sub_sqrt() -> None:
    a = [0.25, 1.0, 2.25, 4.0, 6.25, 9.0, 12.25, 16.0]
    e = 0.0
    for j in range(8):
        e = e - math.sqrt(a[j] * a[j])
    print(e)


def _case_sub_shared() -> None:
    a = [0.5, 1.5, -2.5, 3.5, 4.5]
    out = [0.0] * 5
    e = 0.0
    for j in range(5):
        x = a[j] * a[j]
        e = e - x
        out[j] = x
    print(e, out)


def _case_sub_mixmag() -> None:
    a = [1e-16, 1.0, 1e-16, 2.0, -3.0, 1e-16]
    s = 1.0
    for j in range(6):
        s = s - a[j]
    print(s)


def _case_int_sub() -> None:
    # int 减号归约：本轮仍走标量（判据只放行 float），只钉正确性。
    a = [3, -1, 5, 2, 8, -4]
    t = 7
    for j in range(6):
        t = t - a[j]
    print(t)


_case_sub(0)
_case_sub(1)
_case_sub(2)
_case_sub(3)
_case_sub(5)
_case_sub(7)
_case_sub_neg()
_case_sub_sqrt()
_case_sub_shared()
_case_sub_mixmag()
_case_int_sub()

# --- expect ---
# 0 0.0
# 1 -0.25
# 2 -1.25
# 3 1.0
# 5 3.25
# 7 -18.0
# 4.0
# -51.0
# -41.25 [0.25, 2.25, 6.25, 12.25, 20.25]
# 0.9999999999999999
# -6
