# pyadamas: vect
# 473: P5b 归纳变量 j 参与整数算术 —— 边界、负数、INT64_MIN、守卫形态。
# 覆盖：
#   * 同一个仿射值既当下标又当算术语（t = j + 1; out[j] = a[t] + t）——
#     折叠规则必须只在「全部使用点都是下标」时生效；
#   * 原地 map（读在写前）、while 形状（增量在体块尾部）、n = 0..9/16；
#   * INT64_MIN / INT64_MAX 参与 j 算术（+j 会回绕）的 // %：与不向量化的
#     反向循环逐位对拍（CPython 不溢出，所以只打印两条实现是否相等）；
#   * float(a[j] + j) 的 i64→double 与标量 cvtsi2sdq 对拍；
#   * 除数不是 2 的幂 / 除数是运行期值 → 分类器维持 Fail（标量），结果仍对；
#   * 数组比 n 少一个 → 入口守卫失败走标量，IndexError 时机不变。

MINV = -9223372036854775807 - 1
MAXV = 9223372036854775807


def mk(n: int) -> list[int]:
    a: list[int] = []
    for i in range(n):
        a.append((i * 7 + 3) % 13 - 6)
    return a


def zeros(n: int) -> list[int]:
    out: list[int] = []
    for i in range(n):
        out.append(0)
    return out


# ── 同一个仿射值既当下标又当算术语 ──
# 直接形态（同一个 vreg 两用）：折叠规则必须只在「全部使用点都是下标」时生效，
# 否则发射期查不到寄存器。这是 P5b 的正确性命门。
def idx_and_arith(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = a[j + 1] + (j + 1)


def idx_and_arith2(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = a[j * 2 + 3] + (j * 2 + 3)


def idx_and_arith3(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = a[j + 1] * 2 + a[j + 1]


# 经临时槽的形态（局部变量 t；下标的解析要穿过槽装载，标量路径照旧正确）
def idx_slot(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        t = j + 1
        out[j] = a[t] + t


# ── 原地 map / while 形状 ──
def inplace_addj(a: list[int], n: int) -> None:
    for j in range(n):
        a[j] = a[j] * 2 + j


def while_addj(a: list[int], out: list[int], n: int) -> None:
    j = 0
    while j < n:
        out[j] = a[j] + j * 3
        j = j + 1


# ── 除数不是 2 的幂 / 运行期除数：整段标量，结果仍要对 ──
def map_div3(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = (a[j] + j) // 3


def map_mod7(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = (a[j] + j) % 7


def map_divvar(a: list[int], out: list[int], n: int, k: int) -> None:
    for j in range(n):
        out[j] = (a[j] + j) // k


# ── INT64_MIN 参与 j 算术（回绕）：向量 vs 反向标量对拍 ──
def fwd_min_div2(a: list[int], n: int) -> int:
    s = 0
    for j in range(n):
        s += (a[j] + j) // 2
    return s


def rev_min_div2(a: list[int], n: int) -> int:
    s = 0
    j = n - 1
    while j >= 0:
        s += (a[j] + j) // 2
        j = j - 1
    return s


def fwd_min_mod4(a: list[int], n: int) -> int:
    s = 0
    for j in range(n):
        s += (a[j] + j * j) % 4
    return s


def rev_min_mod4(a: list[int], n: int) -> int:
    s = 0
    j = n - 1
    while j >= 0:
        s += (a[j] + j * j) % 4
        j = j - 1
    return s


def fwd_min_mul(a: list[int], n: int) -> int:
    s = 0
    for j in range(n):
        s += (a[j] + j) * (j + 1) // 8
    return s


def rev_min_mul(a: list[int], n: int) -> int:
    s = 0
    j = n - 1
    while j >= 0:
        s += (a[j] + j) * (j + 1) // 8
        j = j - 1
    return s


def fwd_float(a: list[int], n: int) -> float:
    s = 0.0
    for j in range(n):
        s += float(a[j] + j)
    return s


def rev_float(a: list[int], n: int) -> float:
    s = 0.0
    j = n - 1
    while j >= 0:
        s += float(a[j] + j)
        j = j - 1
    return s


for n in (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 16):
    a = mk(3 * n + 5)
    o = zeros(n)
    idx_and_arith(a, o, n)
    print("ia", n, o)
    o = zeros(n)
    idx_and_arith2(a, o, n)
    print("ia2", n, o)
    o = zeros(n)
    idx_and_arith3(a, o, n)
    print("ia3", n, o)
    o = zeros(n)
    idx_slot(a, o, n)
    print("ias", n, o)
    ip = mk(3 * n + 5)
    inplace_addj(ip, n)
    print("ip", n, ip)
    o = zeros(n)
    while_addj(a, o, n)
    print("wa", n, o)
    o = zeros(n)
    map_div3(a, o, n)
    print("d3", n, o)
    o = zeros(n)
    map_mod7(a, o, n)
    print("p7", n, o)
    o = zeros(n)
    map_divvar(a, o, n, 5)
    print("dv", n, o)
    print("fmin2", n, fwd_min_div2(a, n) == rev_min_div2(a, n))
    print("fmin4", n, fwd_min_mod4(a, n) == rev_min_mod4(a, n))
    print("fminm", n, fwd_min_mul(a, n) == rev_min_mul(a, n))
    print("fflt", n, fwd_float(a, n) == rev_float(a, n))

# INT64_MIN / INT64_MAX / 回绕边界：与反向标量对拍（含 n 恰好等于数组长度）
edge: list[int] = [MINV, MINV + 1, MINV + 2, -3, -2, -1, 0, 1, 2, 3, 5, 6, 7,
                   MAXV - 1, MAXV]
for n in (0, 1, 2, 3, 4, 5, 8, 15):
    print("emin2", n, fwd_min_div2(edge, n) == rev_min_div2(edge, n))
    print("emin4", n, fwd_min_mod4(edge, n) == rev_min_mod4(edge, n))
    print("eminm", n, fwd_min_mul(edge, n) == rev_min_mul(edge, n))
    print("eflt", n, fwd_float(edge, n) == rev_float(edge, n))

# 越界：数组比 n 少一个 → 守卫失败走标量，IndexError 时机不变
for n in (4, 5, 8):  # IndexError 时机
    a = mk(n - 1)
    o = zeros(n)
    try:
        idx_and_arith(a, o, n)
        print("oob", n, "no error", o)
    except IndexError:
        print("oob", n, "IndexError", o)

# --- expect ---
# ia 0 []
# ia2 0 []
# ia3 0 []
# ias 0 []
# ip 0 [-3, 4, -2, 5, -1]
# wa 0 []
# d3 0 []
# p7 0 []
# dv 0 []
# fmin2 0 True
# fmin4 0 True
# fminm 0 True
# fflt 0 True
# ia 1 [5]
# ia2 1 [8]
# ia3 1 [12]
# ias 1 [5]
# ip 1 [-6, 4, -2, 5, -1, 6, 0, -6]
# wa 1 [-3]
# d3 1 [-1]
# p7 1 [4]
# dv 1 [-1]
# fmin2 1 True
# fmin4 1 True
# fminm 1 True
# fflt 1 True
# ia 2 [5, 0]
# ia2 2 [8, 11]
# ia3 2 [12, -6]
# ias 2 [5, 0]
# ip 2 [-6, 9, -2, 5, -1, 6, 0, -6, 1, -5, 2]
# wa 2 [-3, 7]
# d3 2 [-1, 1]
# p7 2 [4, 5]
# dv 2 [-1, 1]
# fmin2 2 True
# fmin4 2 True
# fminm 2 True
# fflt 2 True
# ia 3 [5, 0, 8]
# ia2 3 [8, 11, 1]
# ia3 3 [12, -6, 15]
# ias 3 [5, 0, 8]
# ip 3 [-6, 9, -2, 5, -1, 6, 0, -6, 1, -5, 2, -4, 3, -3]
# wa 3 [-3, 7, 4]
# d3 3 [-1, 1, 0]
# p7 3 [4, 5, 0]
# dv 3 [-1, 1, 0]
# fmin2 3 True
# fmin4 3 True
# fminm 3 True
# fflt 3 True
# ia 4 [5, 0, 8, 3]
# ia2 4 [8, 11, 1, 4]
# ia3 4 [12, -6, 15, -3]
# ias 4 [5, 0, 8, 3]
# ip 4 [-6, 9, -2, 13, -1, 6, 0, -6, 1, -5, 2, -4, 3, -3, 4, -2, 5]
# wa 4 [-3, 7, 4, 14]
# d3 4 [-1, 1, 0, 2]
# p7 4 [4, 5, 0, 1]
# dv 4 [-1, 1, 0, 1]
# fmin2 4 True
# fmin4 4 True
# fminm 4 True
# fflt 4 True
# ia 5 [5, 0, 8, 3, 11]
# ia2 5 [8, 11, 1, 4, 7]
# ia3 5 [12, -6, 15, -3, 18]
# ias 5 [5, 0, 8, 3, 11]
# ip 5 [-6, 9, -2, 13, 2, 6, 0, -6, 1, -5, 2, -4, 3, -3, 4, -2, 5, -1, 6, 0]
# wa 5 [-3, 7, 4, 14, 11]
# d3 5 [-1, 1, 0, 2, 1]
# p7 5 [4, 5, 0, 1, 3]
# dv 5 [-1, 1, 0, 1, 0]
# fmin2 5 True
# fmin4 5 True
# fminm 5 True
# fflt 5 True
# ia 6 [5, 0, 8, 3, 11, 6]
# ia2 6 [8, 11, 1, 4, 7, 10]
# ia3 6 [12, -6, 15, -3, 18, 0]
# ias 6 [5, 0, 8, 3, 11, 6]
# ip 6 [-6, 9, -2, 13, 2, 17, 0, -6, 1, -5, 2, -4, 3, -3, 4, -2, 5, -1, 6, 0, -6, 1, -5]
# wa 6 [-3, 7, 4, 14, 11, 21]
# d3 6 [-1, 1, 0, 2, 1, 3]
# p7 6 [4, 5, 0, 1, 3, 4]
# dv 6 [-1, 1, 0, 1, 0, 2]
# fmin2 6 True
# fmin4 6 True
# fminm 6 True
# fflt 6 True
# ia 7 [5, 0, 8, 3, 11, 6, 1]
# ia2 7 [8, 11, 1, 4, 7, 10, 13]
# ia3 7 [12, -6, 15, -3, 18, 0, -18]
# ias 7 [5, 0, 8, 3, 11, 6, 1]
# ip 7 [-6, 9, -2, 13, 2, 17, 6, -6, 1, -5, 2, -4, 3, -3, 4, -2, 5, -1, 6, 0, -6, 1, -5, 2, -4, 3]
# wa 7 [-3, 7, 4, 14, 11, 21, 18]
# d3 7 [-1, 1, 0, 2, 1, 3, 2]
# p7 7 [4, 5, 0, 1, 3, 4, 6]
# dv 7 [-1, 1, 0, 1, 0, 2, 1]
# fmin2 7 True
# fmin4 7 True
# fminm 7 True
# fflt 7 True
# ia 8 [5, 0, 8, 3, 11, 6, 1, 9]
# ia2 8 [8, 11, 1, 4, 7, 10, 13, 16]
# ia3 8 [12, -6, 15, -3, 18, 0, -18, 3]
# ias 8 [5, 0, 8, 3, 11, 6, 1, 9]
# ip 8 [-6, 9, -2, 13, 2, 17, 6, -5, 1, -5, 2, -4, 3, -3, 4, -2, 5, -1, 6, 0, -6, 1, -5, 2, -4, 3, -3, 4, -2]
# wa 8 [-3, 7, 4, 14, 11, 21, 18, 15]
# d3 8 [-1, 1, 0, 2, 1, 3, 2, 0]
# p7 8 [4, 5, 0, 1, 3, 4, 6, 1]
# dv 8 [-1, 1, 0, 1, 0, 2, 1, 0]
# fmin2 8 True
# fmin4 8 True
# fminm 8 True
# fflt 8 True
# ia 9 [5, 0, 8, 3, 11, 6, 1, 9, 4]
# ia2 9 [8, 11, 1, 4, 7, 10, 13, 16, 19]
# ia3 9 [12, -6, 15, -3, 18, 0, -18, 3, -15]
# ias 9 [5, 0, 8, 3, 11, 6, 1, 9, 4]
# ip 9 [-6, 9, -2, 13, 2, 17, 6, -5, 10, -5, 2, -4, 3, -3, 4, -2, 5, -1, 6, 0, -6, 1, -5, 2, -4, 3, -3, 4, -2, 5, -1, 6]
# wa 9 [-3, 7, 4, 14, 11, 21, 18, 15, 25]
# d3 9 [-1, 1, 0, 2, 1, 3, 2, 0, 3]
# p7 9 [4, 5, 0, 1, 3, 4, 6, 1, 2]
# dv 9 [-1, 1, 0, 1, 0, 2, 1, 0, 1]
# fmin2 9 True
# fmin4 9 True
# fminm 9 True
# fflt 9 True
# ia 16 [5, 0, 8, 3, 11, 6, 1, 9, 4, 12, 7, 15, 10, 18, 13, 21]
# ia2 16 [8, 11, 1, 4, 7, 10, 13, 16, 19, 22, 25, 28, 31, 34, 37, 27]
# ia3 16 [12, -6, 15, -3, 18, 0, -18, 3, -15, 6, -12, 9, -9, 12, -6, 15]
# ias 16 [5, 0, 8, 3, 11, 6, 1, 9, 4, 12, 7, 15, 10, 18, 13, 21]
# ip 16 [-6, 9, -2, 13, 2, 17, 6, -5, 10, -1, 14, 3, 18, 7, 22, 11, 5, -1, 6, 0, -6, 1, -5, 2, -4, 3, -3, 4, -2, 5, -1, 6, 0, -6, 1, -5, 2, -4, 3, -3, 4, -2, 5, -1, 6, 0, -6, 1, -5, 2, -4, 3, -3]
# wa 16 [-3, 7, 4, 14, 11, 21, 18, 15, 25, 22, 32, 29, 39, 36, 46, 43]
# d3 16 [-1, 1, 0, 2, 1, 3, 2, 0, 3, 1, 4, 2, 5, 3, 6, 4]
# p7 16 [4, 5, 0, 1, 3, 4, 6, 1, 2, 4, 5, 0, 1, 3, 4, 6]
# dv 16 [-1, 1, 0, 1, 0, 2, 1, 0, 1, 0, 2, 1, 3, 2, 3, 2]
# fmin2 16 True
# fmin4 16 True
# fminm 16 True
# fflt 16 True
# emin2 0 True
# emin4 0 True
# eminm 0 True
# eflt 0 True
# emin2 1 True
# emin4 1 True
# eminm 1 True
# eflt 1 True
# emin2 2 True
# emin4 2 True
# eminm 2 True
# eflt 2 True
# emin2 3 True
# emin4 3 True
# eminm 3 True
# eflt 3 True
# emin2 4 True
# emin4 4 True
# eminm 4 True
# eflt 4 True
# emin2 5 True
# emin4 5 True
# eminm 5 True
# eflt 5 True
# emin2 8 True
# emin4 8 True
# eminm 8 True
# eflt 8 True
# emin2 15 True
# emin4 15 True
# eminm 15 True
# eflt 15 True
# oob 4 IndexError [5, 0, 0, 0]
# oob 5 IndexError [5, 0, 8, 0, 0]
# oob 8 IndexError [5, 0, 8, 3, 11, 6, 0, 0]
