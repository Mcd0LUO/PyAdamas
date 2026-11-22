# pyadamas: vect
# math.sqrt 的通用 2-lane 向量化回归（阶段 B2）：
#   * out[j]=sqrt(a[j]) / sqrt(a[j]^2+b[j]^2) / s+=sqrt(...) 走 2-lane sqrtsd
#     （低/高 lane 分别 sqrtsd + movlhps 拼回）；
#   * 负数 lane → 守卫跳标量尾：ValueError 的抛出时机与标量路径一致，
#     绝不能静默返回 NaN；
#   * sqrt 之前有 map 写或 reduction 写 → 整段保守标量（守卫退出到标量尾时，
#     相邻 lane 的写/累加不能提前泄漏）；这两段只钉正确性。
# 每段独立函数，避免同名变量/参数跨段冲突；输出对拍 CPython。
import math


def _case_map() -> None:
    a = [0.25, 1.0, 2.25, 4.0, 6.25, 9.0, 12.25, 16.0]
    out = [0.0] * 8
    for j in range(8):
        out[j] = math.sqrt(a[j])
    print(out)


def _case_dist() -> None:
    a = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0]
    b = [1.0, 1.0, 2.0, 2.0, 3.0, 3.0, 4.0, 4.0]
    out = [0.0] * 8
    for j in range(8):
        out[j] = math.sqrt(a[j] * a[j] + b[j] * b[j])
    print(out)


def _case_reduce() -> None:
    a = [0.25, 1.0, 2.25, 4.0, 6.25, 9.0, 12.25, 16.0]
    s = 0.0
    for j in range(8):
        s = s + math.sqrt(a[j] * a[j])
    print(s)


def _case_temp_slot() -> None:
    a = [1.0, 4.0, 9.0, 16.0, 25.0, 36.0, 49.0, 64.0]
    out = [0.0] * 8
    for j in range(8):
        d = a[j] * a[j]
        out[j] = math.sqrt(d) + 0.5
    print(out)


def _case_negative() -> None:
    a = [1.0, 4.0, -1.0, 9.0]
    out = [0.0] * 4
    try:
        for j in range(4):
            out[j] = math.sqrt(a[j])
        print("no error", out)
    except ValueError:
        print("caught")


def _case_store_before_sqrt() -> None:
    # sqrt 之前有 map 写：必须保守标量（否则守卫退出时相邻 lane 的 out[3] 会泄漏）。
    a = [1.0, 4.0, -1.0, 9.0]
    out = [0.0] * 4
    out2 = [0.0] * 4
    try:
        for j in range(4):
            out[j] = a[j] + 1.0
            out2[j] = math.sqrt(a[j])
        print("no error")
    except ValueError:
        print("caught", out)


def _case_reduce_then_sqrt() -> None:
    # reduction 写之后再有 sqrt：必须保守标量（守卫退出会重复累加）。
    a = [0.25, 1.0, 2.25, 4.0]
    out = [0.0] * 4
    s = 0.0
    for j in range(4):
        s = s + a[j]
        out[j] = math.sqrt(a[j] * a[j])
    print(s, out)


_case_map()
_case_dist()
_case_reduce()
_case_temp_slot()
_case_negative()
_case_store_before_sqrt()
_case_reduce_then_sqrt()

# --- expect ---
# [0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0]
# [1.4142135623730951, 2.23606797749979, 3.605551275463989, 4.47213595499958, 5.830951894845301, 6.708203932499369, 8.06225774829855, 8.94427190999916]
# 51.0
# [1.5, 4.5, 9.5, 16.5, 25.5, 36.5, 49.5, 64.5]
# caught
# caught [2.0, 5.0, 0.0, 0.0]
# 7.5 [0.25, 1.0, 2.25, 4.0]
