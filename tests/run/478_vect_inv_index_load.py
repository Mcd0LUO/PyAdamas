# pyadamas: vect
# 循环内不变下标元素装载（仿射 coef==0 → 2-lane 广播装载）回归（阶段 B4）：
#   * b[j] = a[i*7+1] + v[j] / a[3] / 两个不变元素参与算式：preheader 之后每
#     lane 读同一个元素 → movsd + movlhps 广播；
#   * 一次性区间守卫：0 <= idx < len，越界跳标量尾 → IndexError 时机与标量一致；
#   * 运行期负下标（i*3-1 = -1）走 Python 从尾部索引语义，必须与 CPython 一致；
#   * n=0/1/2/3/5（含奇数尾）、float 与 int 两种元素类型。
import math


def _case_float(a: list[float], v: list[float], b: list[float], i: int, n: int) -> None:
    for j in range(n):
        b[j] = a[i * 7 + 1] + v[j]


def _case_float_two(a: list[float], v: list[float], b: list[float], i: int, n: int) -> None:
    for j in range(n):
        b[j] = a[i * 7] * a[i * 7 + 1] + v[j] * 2.0


def _case_const_idx(a: list[float], v: list[float], b: list[float], n: int) -> None:
    for j in range(n):
        b[j] = a[3] + v[j]


def _case_sqrt_inv(a: list[float], v: list[float], b: list[float], i: int, n: int) -> None:
    for j in range(n):
        b[j] = math.sqrt(a[i * 7] * a[i * 7]) + v[j]


def _case_int(a: list[int], v: list[int], b: list[int], i: int, n: int) -> None:
    for j in range(n):
        b[j] = a[i * 7 + 1] + v[j]


def _case_oob(a: list[float], v: list[float], b: list[float], i: int, n: int) -> None:
    for j in range(n):
        b[j] = a[i * 7 + 1] + v[j]


def _case_neg(a: list[float], v: list[float], b: list[float], i: int, n: int) -> None:
    for j in range(n):
        b[j] = a[i * 3 - 1] + v[j]


a = [float(k) + 0.5 for k in range(35)]
v = [0.5, 1.5, 2.5, 3.5, 4.5]
ai = [k * 3 + 1 for k in range(35)]
vi = [10, 20, 30, 40, 50]

for n in (0, 1, 2, 3, 5):
    b = [0.0] * 5
    _case_float(a, v, b, 2, n)
    print("f", n, b)
    b = [0.0] * 5
    _case_float_two(a, v, b, 2, n)
    print("g", n, b)
    b = [0.0] * 5
    _case_const_idx(a, v, b, n)
    print("c", n, b)
    b = [0.0] * 5
    _case_sqrt_inv(a, v, b, 2, n)
    print("s", n, b)
    bi = [0] * 5
    _case_int(ai, vi, bi, 2, n)
    print("i", n, bi)

# 不变下标越界：IndexError 的时机/行为与标量一致（向量路径守卫失败 → 标量尾）。
b = [0.0] * 4
try:
    _case_oob([1.0, 2.0, 3.0], v, b, 2, 4)
    print("no error", b)
except IndexError:
    print("IndexError")

# 运行期负下标：Python 从尾部索引（a[-1]），必须与 CPython 一致。
b = [0.0] * 4
_case_neg(a, v, b, 0, 4)
print("neg", b)

# 负下标但越界（a[-100]）→ IndexError。
b = [0.0] * 4
try:
    _case_neg(a, v, b, -33, 4)
    print("no error2", b)
except IndexError:
    print("IndexError2")

# --- expect ---
# f 0 [0.0, 0.0, 0.0, 0.0, 0.0]
# g 0 [0.0, 0.0, 0.0, 0.0, 0.0]
# c 0 [0.0, 0.0, 0.0, 0.0, 0.0]
# s 0 [0.0, 0.0, 0.0, 0.0, 0.0]
# i 0 [0, 0, 0, 0, 0]
# f 1 [16.0, 0.0, 0.0, 0.0, 0.0]
# g 1 [225.75, 0.0, 0.0, 0.0, 0.0]
# c 1 [4.0, 0.0, 0.0, 0.0, 0.0]
# s 1 [15.0, 0.0, 0.0, 0.0, 0.0]
# i 1 [56, 0, 0, 0, 0]
# f 2 [16.0, 17.0, 0.0, 0.0, 0.0]
# g 2 [225.75, 227.75, 0.0, 0.0, 0.0]
# c 2 [4.0, 5.0, 0.0, 0.0, 0.0]
# s 2 [15.0, 16.0, 0.0, 0.0, 0.0]
# i 2 [56, 66, 0, 0, 0]
# f 3 [16.0, 17.0, 18.0, 0.0, 0.0]
# g 3 [225.75, 227.75, 229.75, 0.0, 0.0]
# c 3 [4.0, 5.0, 6.0, 0.0, 0.0]
# s 3 [15.0, 16.0, 17.0, 0.0, 0.0]
# i 3 [56, 66, 76, 0, 0]
# f 5 [16.0, 17.0, 18.0, 19.0, 20.0]
# g 5 [225.75, 227.75, 229.75, 231.75, 233.75]
# c 5 [4.0, 5.0, 6.0, 7.0, 8.0]
# s 5 [15.0, 16.0, 17.0, 18.0, 19.0]
# i 5 [56, 66, 76, 86, 96]
# IndexError
# neg [35.0, 36.0, 37.0, 38.0]
# IndexError2
