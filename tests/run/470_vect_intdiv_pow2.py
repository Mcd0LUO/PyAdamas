# pyadamas: vect
# 470: P5 整数 // 与 % 的 2 的幂除数 —— 向量化回归（map + reduction）。
# 判据只有「除数是编译期正的 2 的幂常量」：这是通用操作支持（标量那边早就是
# 一条 sarq / andq，LLVM InstCombine / GCC tree-vect 对 sdiv 2^k 同样处理），
# 不含任何基准名或形状特判。
#
# 语义参照：x // 2^m == 算术右移（floor，含 INT64_MIN）；x % 2^m == 低 m 位
# （Python 的 % 与除数同号，正除数下负数也成立：-7 % 4 == 1）。
# 覆盖：// 2 / 4 / 8 / 1024 与 % 2 / 4 / 1024 的 int map 与 int reduction；
# 含负数、含 INT64_MIN / INT64_MIN+1 / INT64_MAX；n = 0/1/2/3/5 覆盖向量循环
# 的入口守卫与标量尾。参考值一律由 CPython 逐字节给出（--- expect ---）。

MINV = -9223372036854775807 - 1
MAXV = 9223372036854775807


# ── int map：out[j] = a[j] // 2^m ──
def map2(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = a[j] // 2


def map4(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = a[j] // 4


def map8(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = a[j] // 8


def map1024(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = a[j] // 1024


# ── int map：out[j] = a[j] % 2^m ──
def mod2(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = a[j] % 2


def mod4(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = a[j] % 4


def mod1024(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = a[j] % 1024


# ── int reduction：s += a[j] // 2^m ──
def red2(a: list[int], n: int) -> int:
    s = 0
    for j in range(n):
        s += a[j] // 2
    return s


def red4(a: list[int], n: int) -> int:
    s = 0
    for j in range(n):
        s += a[j] // 4
    return s


def red8(a: list[int], n: int) -> int:
    s = 0
    for j in range(n):
        s += a[j] // 8
    return s


def red1024(a: list[int], n: int) -> int:
    s = 0
    for j in range(n):
        s += a[j] // 1024
    return s


# ── int reduction：s += a[j] % 2^m ──
def redmod2(a: list[int], n: int) -> int:
    s = 0
    for j in range(n):
        s += a[j] % 2
    return s


def redmod1024(a: list[int], n: int) -> int:
    s = 0
    for j in range(n):
        s += a[j] % 1024
    return s


# ── 非 2 的幂除数：分类器必须维持 Fail（标量），结果同样要与 CPython 一致 ──
def map3(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = a[j] // 3


def mod7(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = a[j] % 7


def zeros(n: int) -> list[int]:
    out: list[int] = []
    for i in range(n):
        out.append(0)
    return out


# 边界值数组：两侧符号、2 的幂边界、INT64_MIN/MAX（// 与 % 都逐位对拍）
edge: list[int] = [MINV, MINV + 1, MINV + 2, -1025, -1024, -1023, -512, -8, -5,
                   -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 7, 8, 511, 512, 1023, 1024,
                   1025, MAXV - 1, MAXV]

for n in (0, 1, 2, 3, 5):
    o = zeros(n)
    map2(edge, o, n)
    print("m2", n, o)
    o = zeros(n)
    map4(edge, o, n)
    print("m4", n, o)
    o = zeros(n)
    map8(edge, o, n)
    print("m8", n, o)
    o = zeros(n)
    map1024(edge, o, n)
    print("m1024", n, o)
    o = zeros(n)
    mod2(edge, o, n)
    print("p2", n, o)
    o = zeros(n)
    mod4(edge, o, n)
    print("p4", n, o)
    o = zeros(n)
    mod1024(edge, o, n)
    print("p1024", n, o)

# 全部边界值都过一遍（n == len(edge)：向量循环吃满 + 标量尾）
o = zeros(len(edge))
map2(edge, o, len(edge))
print("M2", o)
o = zeros(len(edge))
map4(edge, o, len(edge))
print("M4", o)
o = zeros(len(edge))
map8(edge, o, len(edge))
print("M8", o)
o = zeros(len(edge))
map1024(edge, o, len(edge))
print("M1024", o)
o = zeros(len(edge))
mod2(edge, o, len(edge))
print("P2", o)
o = zeros(len(edge))
mod4(edge, o, len(edge))
print("P4", o)
o = zeros(len(edge))
mod1024(edge, o, len(edge))
print("P1024", o)

# ── reduction：小值域（不溢出 int64，可与 CPython 逐字节比较）──
small: list[int] = []
for i in range(37):
    small.append((i * 13 + 5) % 29 - 14)

for n in (0, 1, 2, 3, 5, 16, 37):
    print("r2", n, red2(small, n), red4(small, n), red8(small, n),
          red1024(small, n), redmod2(small, n), redmod1024(small, n))

# ── 非 2 的幂除数：整段标量 fallback，结果与 CPython 一致 ──
for n in (0, 1, 2, 3, 5, len(edge)):
    o = zeros(n)
    map3(edge, o, n)
    print("m3", n, o)
    o = zeros(n)
    mod7(edge, o, n)
    print("p7", n, o)

# --- expect ---
# m2 0 []
# m4 0 []
# m8 0 []
# m1024 0 []
# p2 0 []
# p4 0 []
# p1024 0 []
# m2 1 [-4611686018427387904]
# m4 1 [-2305843009213693952]
# m8 1 [-1152921504606846976]
# m1024 1 [-9007199254740992]
# p2 1 [0]
# p4 1 [0]
# p1024 1 [0]
# m2 2 [-4611686018427387904, -4611686018427387904]
# m4 2 [-2305843009213693952, -2305843009213693952]
# m8 2 [-1152921504606846976, -1152921504606846976]
# m1024 2 [-9007199254740992, -9007199254740992]
# p2 2 [0, 1]
# p4 2 [0, 1]
# p1024 2 [0, 1]
# m2 3 [-4611686018427387904, -4611686018427387904, -4611686018427387903]
# m4 3 [-2305843009213693952, -2305843009213693952, -2305843009213693952]
# m8 3 [-1152921504606846976, -1152921504606846976, -1152921504606846976]
# m1024 3 [-9007199254740992, -9007199254740992, -9007199254740992]
# p2 3 [0, 1, 0]
# p4 3 [0, 1, 2]
# p1024 3 [0, 1, 2]
# m2 5 [-4611686018427387904, -4611686018427387904, -4611686018427387903, -513, -512]
# m4 5 [-2305843009213693952, -2305843009213693952, -2305843009213693952, -257, -256]
# m8 5 [-1152921504606846976, -1152921504606846976, -1152921504606846976, -129, -128]
# m1024 5 [-9007199254740992, -9007199254740992, -9007199254740992, -2, -1]
# p2 5 [0, 1, 0, 1, 0]
# p4 5 [0, 1, 2, 3, 0]
# p1024 5 [0, 1, 2, 1023, 0]
# M2 [-4611686018427387904, -4611686018427387904, -4611686018427387903, -513, -512, -512, -256, -4, -3, -2, -2, -1, -1, 0, 0, 1, 1, 2, 2, 3, 4, 255, 256, 511, 512, 512, 4611686018427387903, 4611686018427387903]
# M4 [-2305843009213693952, -2305843009213693952, -2305843009213693952, -257, -256, -256, -128, -2, -2, -1, -1, -1, -1, 0, 0, 0, 0, 1, 1, 1, 2, 127, 128, 255, 256, 256, 2305843009213693951, 2305843009213693951]
# M8 [-1152921504606846976, -1152921504606846976, -1152921504606846976, -129, -128, -128, -64, -1, -1, -1, -1, -1, -1, 0, 0, 0, 0, 0, 0, 0, 1, 63, 64, 127, 128, 128, 1152921504606846975, 1152921504606846975]
# M1024 [-9007199254740992, -9007199254740992, -9007199254740992, -2, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 9007199254740991, 9007199254740991]
# P2 [0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 1]
# P4 [0, 1, 2, 3, 0, 1, 0, 0, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 3, 0, 3, 0, 3, 0, 1, 2, 3]
# P1024 [0, 1, 2, 1023, 0, 1, 512, 1016, 1019, 1020, 1021, 1022, 1023, 0, 1, 2, 3, 4, 5, 7, 8, 511, 512, 1023, 0, 1, 1022, 1023]
# r2 0 0 0 0 0 0 0
# r2 1 -5 -3 -2 -1 1 1015
# r2 2 -3 -2 -2 -1 1 1019
# r2 3 -9 -5 -4 -2 1 2031
# r2 5 -2 -2 -3 -2 2 2046
# r2 16 -6 -7 -8 -8 7 8187
# r2 37 -8 -14 -17 -18 18 18434
# m3 0 []
# p7 0 []
# m3 1 [-3074457345618258603]
# p7 1 [6]
# m3 2 [-3074457345618258603, -3074457345618258603]
# p7 2 [6, 0]
# m3 3 [-3074457345618258603, -3074457345618258603, -3074457345618258602]
# p7 3 [6, 0, 1]
# m3 5 [-3074457345618258603, -3074457345618258603, -3074457345618258602, -342, -342]
# p7 5 [6, 0, 1, 4, 5]
# m3 28 [-3074457345618258603, -3074457345618258603, -3074457345618258602, -342, -342, -341, -171, -3, -2, -2, -1, -1, -1, 0, 0, 0, 1, 1, 1, 2, 2, 170, 170, 341, 341, 341, 3074457345618258602, 3074457345618258602]
# p7 28 [6, 0, 1, 4, 5, 6, 6, 6, 2, 3, 4, 5, 6, 0, 1, 2, 3, 4, 5, 0, 1, 0, 1, 1, 2, 3, 6, 0]
