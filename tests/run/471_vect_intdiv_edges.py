# pyadamas: vect
# 471: P5 整数 // 与 % 的 2 的幂除数 —— 边界与守卫回归。
# 覆盖：
#   * 恰好长度（4-lane 循环吃满到数组末尾）与 n = 0..9 的入口守卫/标量尾；
#   * 原地 map（同对象同偏移）、带初值的 reduction、while 形状；
#   * 被除数不是裸装载而是算式（a[j]*3+7、a[j]*a[j]、a[j]+b[j]）—— 判据是
#     「除数是 2 的幂常量」，与被除数形状无关；
#   * // 1（m == 0 的恒等）与 % 1（恒 0）；
#   * 除数不是 2 的幂 / 除数是运行期值 → 分类器维持 Fail（标量），结果仍正确；
#   * int // 2^k 的结果送进 float（Convert）—— 2-lane 浮点路径照旧。
# INT64_MIN 的 reduction 用**不向量化**的反向循环做参考对拍（避免两边同一个
# bug），其余参考值由 CPython 逐字节给出。

MINV = -9223372036854775807 - 1


def mk(n: int) -> list[int]:
    a: list[int] = []
    for i in range(n):
        a.append((i * 11 + 3) % 17 - 8)
    return a


def zeros(n: int) -> list[int]:
    out: list[int] = []
    for i in range(n):
        out.append(0)
    return out


# ── 恰好长度 / 原地 / 算式被除数 ──
def exact_div4(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = a[j] // 4


def inplace_div8(a: list[int], n: int) -> None:
    for j in range(n):
        a[j] = a[j] // 8


def expr_div16(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = (a[j] * 3 + 7) // 16


def sqr_div1024(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = a[j] * a[j] // 1024


def add_div4(a: list[int], b: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = (a[j] + b[j]) // 4


def expr_mod8(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = (a[j] * 5 - 3) % 8


# ── 带初值的 reduction / while 形状 ──
def red_init(a: list[int], n: int, t: int) -> int:
    s = t
    for j in range(n):
        s += a[j] // 2
    return s


def while_div4(a: list[int], out: list[int], n: int) -> None:
    j = 0
    while j < n:
        out[j] = a[j] // 4
        j = j + 1


def while_red(a: list[int], n: int) -> int:
    j = 0
    s = 0
    while j < n:
        s += a[j] % 8
        j = j + 1
    return s


# ── // 1 与 % 1 ──
def div1(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = a[j] // 1


def mod1(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = a[j] % 1


# ── 负的 2 的幂除数：标量那边是另一套式子（trunc + 符号还原），分类器必须
#    维持 Fail（不走移位合成），结果照样与 CPython 一致 ──
def divneg2(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = a[j] // -2


def modneg4(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = a[j] % -4


# ── 除数不是 2 的幂 / 运行期除数：必须走标量（结果仍要对）──
def div3(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = a[j] // 3


def divvar(a: list[int], out: list[int], n: int, k: int) -> None:
    for j in range(n):
        out[j] = a[j] // k


def modvar(a: list[int], out: list[int], n: int, k: int) -> None:
    for j in range(n):
        out[j] = a[j] % k


# ── int // 2^k 送进 float（2-lane 浮点路径）──
def float_chain(a: list[int], n: int) -> float:
    t = 0.0
    for j in range(n):
        t += 1.0 / (a[j] // 8 + 40)
    return t


# ── 参考：反向循环（向量化器不认，作为 INT64_MIN reduction 的独立参照）──
def ref_rev_div2(a: list[int], n: int) -> int:
    s = 0
    j = n - 1
    while j >= 0:
        s += a[j] // 2
        j = j - 1
    return s


def fwd_div2(a: list[int], n: int) -> int:
    s = 0
    for j in range(n):
        s += a[j] // 2
    return s


for n in (0, 1, 2, 3, 4, 5, 6, 7, 8, 9):
    a = mk(n)
    b = mk(n + 1)
    out = zeros(n)
    exact_div4(a, out, n)
    print("d4", n, out)
    ip = mk(n + 2)
    inplace_div8(ip, n)
    print("ip8", n, ip)
    o2 = zeros(n)
    expr_div16(a, o2, n)
    print("e16", n, o2)
    o3 = zeros(n)
    sqr_div1024(a, o3, n)
    print("sq1024", n, o3)
    o4 = zeros(n)
    add_div4(a, b, o4, n)
    print("a4", n, o4)
    o5 = zeros(n)
    expr_mod8(a, o5, n)
    print("e8", n, o5)
    print("ri", n, red_init(a, n, 12345))
    o6 = zeros(n)
    while_div4(a, o6, n)
    print("w4", n, o6)
    print("wr", n, while_red(a, n))
    o7 = zeros(n)
    div1(a, o7, n)
    print("d1", n, o7)
    o8 = zeros(n)
    mod1(a, o8, n)
    print("p1", n, o8)
    o9 = zeros(n)
    div3(a, o9, n)
    print("d3", n, o9)
    oc = zeros(n)
    divneg2(a, oc, n)
    print("dn2", n, oc)
    od = zeros(n)
    modneg4(a, od, n)
    print("pn4", n, od)
    oa = zeros(n)
    divvar(a, oa, n, 8)
    print("dv", n, oa)
    ob = zeros(n)
    modvar(a, ob, n, 8)
    print("pv", n, ob)
    print("fc", n, format(float_chain(a, n), ".6f"))

# INT64_MIN 的 // 2 逐位对拍（向量循环 vs 反向标量参考）
edge: list[int] = [MINV, MINV + 1, MINV + 2, -3, -2, -1, 0, 1, 2, 3, 5, 6, 7,
                   9223372036854775806, 9223372036854775807]
for n in (0, 1, 2, 3, 4, 5, 8, 15):
    print("min", n, fwd_div2(edge, n) == ref_rev_div2(edge, n))

# 恰好长度：4-lane 循环跑到数组末尾（数组长度 == n）
for n in (4, 5, 6, 7, 8, 9, 12):
    a = mk(n)
    out = zeros(n)
    exact_div4(a, out, n)
    print("ex", n, out)

# 越界：数组比 n 少一个 → 守卫失败走标量，IndexError 时机不变
for n in (4, 5, 8):
    a = mk(n - 1)
    out = zeros(n)
    try:
        exact_div4(a, out, n)
        print("oob", n, "no error", out)
    except IndexError:
        print("oob", n, "IndexError", out)

# --- expect ---
# d4 0 []
# ip8 0 [-5, 6]
# e16 0 []
# sq1024 0 []
# a4 0 []
# e8 0 []
# ri 0 12345
# w4 0 []
# wr 0 0
# d1 0 []
# p1 0 []
# d3 0 []
# dn2 0 []
# pn4 0 []
# dv 0 []
# pv 0 []
# fc 0 0.000000
# d4 1 [-2]
# ip8 1 [-1, 6, 0]
# e16 1 [-1]
# sq1024 1 [0]
# a4 1 [-3]
# e8 1 [4]
# ri 1 12342
# w4 1 [-2]
# wr 1 3
# d1 1 [-5]
# p1 1 [0]
# d3 1 [-2]
# dn2 1 [2]
# pn4 1 [-1]
# dv 1 [-1]
# pv 1 [3]
# fc 1 0.025641
# d4 2 [-2, 1]
# ip8 2 [-1, 0, 0, -6]
# e16 2 [-1, 1]
# sq1024 2 [0, 0]
# a4 2 [-3, 3]
# e8 2 [4, 3]
# ri 2 12345
# w4 2 [-2, 1]
# wr 2 9
# d1 2 [-5, 6]
# p1 2 [0, 0]
# d3 2 [-2, 2]
# dn2 2 [2, -3]
# pn4 2 [-1, -2]
# dv 2 [-1, 0]
# pv 2 [3, 6]
# fc 2 0.050641
# d4 3 [-2, 1, 0]
# ip8 3 [-1, 0, 0, -6, 5]
# e16 3 [-1, 1, 0]
# sq1024 3 [0, 0, 0]
# a4 3 [-3, 3, 0]
# e8 3 [4, 3, 5]
# ri 3 12345
# w4 3 [-2, 1, 0]
# wr 3 9
# d1 3 [-5, 6, 0]
# p1 3 [0, 0, 0]
# d3 3 [-2, 2, 0]
# dn2 3 [2, -3, 0]
# pn4 3 [-1, -2, 0]
# dv 3 [-1, 0, 0]
# pv 3 [3, 6, 0]
# fc 3 0.075641
# d4 4 [-2, 1, 0, -2]
# ip8 4 [-1, 0, 0, -1, 5, -1]
# e16 4 [-1, 1, 0, -1]
# sq1024 4 [0, 0, 0, 0]
# a4 4 [-3, 3, 0, -3]
# e8 4 [4, 3, 5, 7]
# ri 4 12342
# w4 4 [-2, 1, 0, -2]
# wr 4 11
# d1 4 [-5, 6, 0, -6]
# p1 4 [0, 0, 0, 0]
# d3 4 [-2, 2, 0, -2]
# dn2 4 [2, -3, 0, 3]
# pn4 4 [-1, -2, 0, -2]
# dv 4 [-1, 0, 0, -1]
# pv 4 [3, 6, 0, 2]
# fc 4 0.101282
# d4 5 [-2, 1, 0, -2, 1]
# ip8 5 [-1, 0, 0, -1, 0, -1, -7]
# e16 5 [-1, 1, 0, -1, 1]
# sq1024 5 [0, 0, 0, 0, 0]
# a4 5 [-3, 3, 0, -3, 2]
# e8 5 [4, 3, 5, 7, 6]
# ri 5 12344
# w4 5 [-2, 1, 0, -2, 1]
# wr 5 16
# d1 5 [-5, 6, 0, -6, 5]
# p1 5 [0, 0, 0, 0, 0]
# d3 5 [-2, 2, 0, -2, 1]
# dn2 5 [2, -3, 0, 3, -3]
# pn4 5 [-1, -2, 0, -2, -3]
# dv 5 [-1, 0, 0, -1, 0]
# pv 5 [3, 6, 0, 2, 5]
# fc 5 0.126282
# d4 6 [-2, 1, 0, -2, 1, -1]
# ip8 6 [-1, 0, 0, -1, 0, -1, -7, 4]
# e16 6 [-1, 1, 0, -1, 1, 0]
# sq1024 6 [0, 0, 0, 0, 0, 0]
# a4 6 [-3, 3, 0, -3, 2, -1]
# e8 6 [4, 3, 5, 7, 6, 0]
# ri 6 12343
# w4 6 [-2, 1, 0, -2, 1, -1]
# wr 6 23
# d1 6 [-5, 6, 0, -6, 5, -1]
# p1 6 [0, 0, 0, 0, 0, 0]
# d3 6 [-2, 2, 0, -2, 1, -1]
# dn2 6 [2, -3, 0, 3, -3, 0]
# pn4 6 [-1, -2, 0, -2, -3, -1]
# dv 6 [-1, 0, 0, -1, 0, -1]
# pv 6 [3, 6, 0, 2, 5, 7]
# fc 6 0.151923
# d4 7 [-2, 1, 0, -2, 1, -1, -2]
# ip8 7 [-1, 0, 0, -1, 0, -1, -1, 4, -2]
# e16 7 [-1, 1, 0, -1, 1, 0, -1]
# sq1024 7 [0, 0, 0, 0, 0, 0, 0]
# a4 7 [-3, 3, 0, -3, 2, -1, -4]
# e8 7 [4, 3, 5, 7, 6, 0, 2]
# ri 7 12339
# w4 7 [-2, 1, 0, -2, 1, -1, -2]
# wr 7 24
# d1 7 [-5, 6, 0, -6, 5, -1, -7]
# p1 7 [0, 0, 0, 0, 0, 0, 0]
# d3 7 [-2, 2, 0, -2, 1, -1, -3]
# dn2 7 [2, -3, 0, 3, -3, 0, 3]
# pn4 7 [-1, -2, 0, -2, -3, -1, -3]
# dv 7 [-1, 0, 0, -1, 0, -1, -1]
# pv 7 [3, 6, 0, 2, 5, 7, 1]
# fc 7 0.177564
# d4 8 [-2, 1, 0, -2, 1, -1, -2, 1]
# ip8 8 [-1, 0, 0, -1, 0, -1, -1, 0, -2, -8]
# e16 8 [-1, 1, 0, -1, 1, 0, -1, 1]
# sq1024 8 [0, 0, 0, 0, 0, 0, 0, 0]
# a4 8 [-3, 3, 0, -3, 2, -1, -4, 2]
# e8 8 [4, 3, 5, 7, 6, 0, 2, 1]
# ri 8 12341
# w4 8 [-2, 1, 0, -2, 1, -1, -2, 1]
# wr 8 28
# d1 8 [-5, 6, 0, -6, 5, -1, -7, 4]
# p1 8 [0, 0, 0, 0, 0, 0, 0, 0]
# d3 8 [-2, 2, 0, -2, 1, -1, -3, 1]
# dn2 8 [2, -3, 0, 3, -3, 0, 3, -2]
# pn4 8 [-1, -2, 0, -2, -3, -1, -3, 0]
# dv 8 [-1, 0, 0, -1, 0, -1, -1, 0]
# pv 8 [3, 6, 0, 2, 5, 7, 1, 4]
# fc 8 0.202564
# d4 9 [-2, 1, 0, -2, 1, -1, -2, 1, -1]
# ip8 9 [-1, 0, 0, -1, 0, -1, -1, 0, -1, -8, 3]
# e16 9 [-1, 1, 0, -1, 1, 0, -1, 1, 0]
# sq1024 9 [0, 0, 0, 0, 0, 0, 0, 0, 0]
# a4 9 [-3, 3, 0, -3, 2, -1, -4, 2, -1]
# e8 9 [4, 3, 5, 7, 6, 0, 2, 1, 3]
# ri 9 12340
# w4 9 [-2, 1, 0, -2, 1, -1, -2, 1, -1]
# wr 9 34
# d1 9 [-5, 6, 0, -6, 5, -1, -7, 4, -2]
# p1 9 [0, 0, 0, 0, 0, 0, 0, 0, 0]
# d3 9 [-2, 2, 0, -2, 1, -1, -3, 1, -1]
# dn2 9 [2, -3, 0, 3, -3, 0, 3, -2, 1]
# pn4 9 [-1, -2, 0, -2, -3, -1, -3, 0, -2]
# dv 9 [-1, 0, 0, -1, 0, -1, -1, 0, -1]
# pv 9 [3, 6, 0, 2, 5, 7, 1, 4, 6]
# fc 9 0.228205
# min 0 True
# min 1 True
# min 2 True
# min 3 True
# min 4 True
# min 5 True
# min 8 True
# min 15 True
# ex 4 [-2, 1, 0, -2]
# ex 5 [-2, 1, 0, -2, 1]
# ex 6 [-2, 1, 0, -2, 1, -1]
# ex 7 [-2, 1, 0, -2, 1, -1, -2]
# ex 8 [-2, 1, 0, -2, 1, -1, -2, 1]
# ex 9 [-2, 1, 0, -2, 1, -1, -2, 1, -1]
# ex 12 [-2, 1, 0, -2, 1, -1, -2, 1, -1, -2, 0, -1]
# oob 4 IndexError [-2, 1, 0, 0]
# oob 5 IndexError [-2, 1, 0, -2, 0]
# oob 8 IndexError [-2, 1, 0, -2, 1, -1, -2, 0]
