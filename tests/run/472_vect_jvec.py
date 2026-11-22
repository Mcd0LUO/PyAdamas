# pyadamas: vect
# 472: P5b 归纳变量 j 作为整数算术的操作数 —— 2-lane/4-lane 与标量逐字节一致。
# 判据只有一条结构性质：操作数是 j-alias 槽的装载（归纳变量本身或其副本槽）。
# 覆盖：
#   * float(j*j)、float(i+j)（i 可负）、float((i+j)*(i+j+1)//2 + i + 1)；
#   * int map：out[j] = a[j]+j*2 / j*j//4 / (a[j]+j*3)%8 / j-a[j] / a[j]-j；
#   * int reduction：s += j*j / (a[j]+j)//8 / (j*j)%4；
#   * 与 // % 的混合形态（除数 2/4/8/16，含 map 与 reduction）；
#   * n = 0/1/2/3/5/7/16/17 覆盖入口守卫、4-lane 吃满、标量尾。
# 参考值由 CPython 逐字节给出（--- expect ---）。


def float_sq(n: int) -> float:
    s = 0.0
    for j in range(n):
        s += float(j * j)
    return s


def float_addj(i: int, n: int) -> float:
    s = 0.0
    for j in range(n):
        s += float(i + j)
    return s


def float_tri(i: int, n: int) -> float:
    s = 0.0
    for j in range(n):
        s += 1.0 / float((i + j) * (i + j + 1) // 2 + i + 1)
    return s


def map_addj(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = a[j] + j * 2


def map_sqdiv(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = (j * j + a[j]) // 4


def map_modj(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = (a[j] + j * 3) % 8


def map_sub_l(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = j - a[j]


def map_sub_r(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = a[j] - j


def map_jdiv(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = j // 8


def map_jmod(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = j * j % 4


def red_jdiv(a: list[int], n: int) -> int:
    s = 0
    for j in range(n):
        s += j // 2
    return s


def red_sq(a: list[int], n: int) -> int:
    s = 0
    for j in range(n):
        s += j * j
    return s


def red_addj_div(a: list[int], n: int) -> int:
    s = 0
    for j in range(n):
        s += (a[j] + j) // 8
    return s


def red_sqmod(a: list[int], n: int) -> int:
    s = 0
    for j in range(n):
        s += (j * j) % 4
    return s


def red_addj_mod(a: list[int], n: int) -> int:
    s = 0
    for j in range(n):
        s += (a[j] + j * 5) % 16
    return s


def zeros(n: int) -> list[int]:
    out: list[int] = []
    for i in range(n):
        out.append(0)
    return out


def mk(n: int) -> list[int]:
    a: list[int] = []
    for i in range(n):
        a.append((i * 13 + 5) % 23 - 11)
    return a


for n in (0, 1, 2, 3, 5, 7, 16, 17):
    a = mk(n + 3)
    for i in (-5, 0, 3, 1000000):
        print("fsq", n, format(float_sq(n), ".17g"))
        print("fadd", i, n, format(float_addj(i, n), ".17g"))
        print("ftri", i, n, format(float_tri(i, n), ".17g"))
    o = zeros(n)
    map_addj(a, o, n)
    print("madd", n, o)
    o = zeros(n)
    map_sqdiv(a, o, n)
    print("msqd", n, o)
    o = zeros(n)
    map_modj(a, o, n)
    print("mmod", n, o)
    o = zeros(n)
    map_sub_l(a, o, n)
    print("msubl", n, o)
    o = zeros(n)
    map_sub_r(a, o, n)
    print("msubr", n, o)
    o = zeros(n)
    map_jdiv(a, o, n)
    print("mjd", n, o)
    o = zeros(n)
    map_jmod(a, o, n)
    print("mjm", n, o)
    print("rjd", n, red_jdiv(a, n))
    print("rsq", n, red_sq(a, n))
    print("radd", n, red_addj_div(a, n))
    print("rsqm", n, red_sqmod(a, n))
    print("raddm", n, red_addj_mod(a, n))

# --- expect ---
# fsq 0 0
# fadd -5 0 0
# ftri -5 0 0
# fsq 0 0
# fadd 0 0 0
# ftri 0 0 0
# fsq 0 0
# fadd 3 0 0
# ftri 3 0 0
# fsq 0 0
# fadd 1000000 0 0
# ftri 1000000 0 0
# madd 0 []
# msqd 0 []
# mmod 0 []
# msubl 0 []
# msubr 0 []
# mjd 0 []
# mjm 0 []
# rjd 0 0
# rsq 0 0
# radd 0 0
# rsqm 0 0
# raddm 0 0
# fsq 1 0
# fadd -5 1 -5
# ftri -5 1 0.16666666666666666
# fsq 1 0
# fadd 0 1 0
# ftri 0 1 1
# fsq 1 0
# fadd 3 1 3
# ftri 3 1 0.10000000000000001
# fsq 1 0
# fadd 1000000 1 1000000
# ftri 1000000 1 1.999994000014e-12
# madd 1 [-6]
# msqd 1 [-2]
# mmod 1 [2]
# msubl 1 [6]
# msubr 1 [-6]
# mjd 1 [0]
# mjm 1 [0]
# rjd 1 0
# rsq 1 0
# radd 1 -1
# rsqm 1 0
# raddm 1 10
# fsq 2 1
# fadd -5 2 -9
# ftri -5 2 0.66666666666666663
# fsq 2 1
# fadd 0 2 1
# ftri 0 2 1.5
# fsq 2 1
# fadd 3 2 7
# ftri 3 2 0.17142857142857143
# fsq 2 1
# fadd 1000000 2 2000001
# ftri 1000000 2 3.9999840000559999e-12
# madd 2 [-6, 9]
# msqd 2 [-2, 2]
# mmod 2 [2, 2]
# msubl 2 [6, -6]
# msubr 2 [-6, 6]
# mjd 2 [0, 0]
# mjm 2 [0, 1]
# rjd 2 0
# rsq 2 1
# radd 2 0
# rsqm 2 1
# raddm 2 22
# fsq 3 5
# fadd -5 3 -12
# ftri -5 3 -0.33333333333333337
# fsq 3 5
# fadd 0 3 3
# ftri 0 3 1.75
# fsq 3 5
# fadd 3 3 12
# ftri 3 3 0.22406015037593985
# fsq 3 5
# fadd 1000000 3 3000003
# ftri 1000000 3 5.9999700001379993e-12
# madd 3 [-6, 9, 1]
# msqd 3 [-2, 2, 0]
# mmod 3 [2, 2, 3]
# msubl 3 [6, -6, 5]
# msubr 3 [-6, 6, -5]
# mjd 3 [0, 0, 0]
# mjm 3 [0, 1, 0]
# rjd 3 1
# rsq 3 5
# radd 3 -1
# rsqm 3 1
# raddm 3 29
# fsq 5 30
# fadd -5 5 -15
# ftri -5 5 -0.91666666666666674
# fsq 5 30
# fadd 0 5 10
# ftri 0 5 1.9837662337662336
# fsq 5 30
# fadd 3 5 25
# ftri 3 5 0.29531015037593983
# fsq 5 30
# fadd 1000000 5 5000010
# ftri 1000000 5 9.9999300004699963e-12
# madd 5 [-6, 9, 1, 16, 8]
# msqd 5 [-2, 2, 0, 4, 4]
# mmod 5 [2, 2, 3, 3, 4]
# msubl 5 [6, -6, 5, -7, 4]
# msubr 5 [-6, 6, -5, 7, -4]
# mjd 5 [0, 0, 0, 0, 0]
# mjm 5 [0, 1, 0, 1, 0]
# rjd 5 4
# rsq 5 30
# radd 5 0
# rsqm 5 2
# raddm 5 42
# fsq 7 91
# fadd -5 7 -14
# ftri -5 7 -1.5
# fsq 7 91
# fadd 0 7 21
# ftri 0 7 2.091720779220779
# fsq 7 91
# fadd 3 7 42
# ftri 3 7 0.340718313641246
# fsq 7 91
# fadd 1000000 7 7000021
# ftri 1000000 7 1.3999874001105991e-11
# madd 7 [-6, 9, 1, 16, 8, 0, 15]
# msqd 7 [-2, 2, 0, 4, 4, 3, 9]
# mmod 7 [2, 2, 3, 3, 4, 5, 5]
# msubl 7 [6, -6, 5, -7, 4, 15, 3]
# msubr 7 [-6, 6, -5, 7, -4, -15, -3]
# mjd 7 [0, 0, 0, 0, 0, 0, 0]
# mjm 7 [0, 1, 0, 1, 0, 1, 0]
# rjd 7 9
# rsq 7 91
# radd 7 0
# rsqm 7 3
# raddm 7 58
# fsq 16 1240
# fadd -5 16 40
# ftri -5 16 -1.5666859593061171
# fsq 16 1240
# fadd 0 16 120
# ftri 0 16 2.2489784294343105
# fsq 16 1240
# fadd 3 16 168
# ftri 3 16 0.43114001587439932
# fsq 16 1240
# fadd 1000000 16 16000120
# ftri 1000000 16 3.1999424010303817e-11
# madd 16 [-6, 9, 1, 16, 8, 0, 15, 7, 22, 14, 29, 21, 13, 28, 20, 35]
# msqd 16 [-2, 2, 0, 4, 4, 3, 9, 10, 17, 19, 27, 30, 33, 42, 47, 57]
# mmod 16 [2, 2, 3, 3, 4, 5, 5, 6, 6, 7, 7, 0, 1, 1, 2, 2]
# msubl 16 [6, -6, 5, -7, 4, 15, 3, 14, 2, 13, 1, 12, 23, 11, 22, 10]
# msubr 16 [-6, 6, -5, 7, -4, -15, -3, -14, -2, -13, -1, -12, -23, -11, -22, -10]
# mjd 16 [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1]
# mjm 16 [0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1]
# rjd 16 56
# rsq 16 1240
# radd 16 7
# rsqm 16 8
# raddm 16 128
# fsq 17 1496
# fadd -5 17 51
# ftri -5 17 -1.5505569270480526
# fsq 17 1496
# fadd 0 17 136
# ftri 0 17 2.2562776995073031
# fsq 17 1496
# fadd 3 17 187
# ftri 3 17 0.43629465504965703
# fsq 17 1496
# fadd 1000000 17 17000136
# ftri 1000000 17 3.3999354012205768e-11
# madd 17 [-6, 9, 1, 16, 8, 0, 15, 7, 22, 14, 29, 21, 13, 28, 20, 35, 27]
# msqd 17 [-2, 2, 0, 4, 4, 3, 9, 10, 17, 19, 27, 30, 33, 42, 47, 57, 62]
# mmod 17 [2, 2, 3, 3, 4, 5, 5, 6, 6, 7, 7, 0, 1, 1, 2, 2, 3]
# msubl 17 [6, -6, 5, -7, 4, 15, 3, 14, 2, 13, 1, 12, 23, 11, 22, 10, 21]
# msubr 17 [-6, 6, -5, 7, -4, -15, -3, -14, -2, -13, -1, -12, -23, -11, -22, -10, -21]
# mjd 17 [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2]
# mjm 17 [0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0]
# rjd 17 64
# rsq 17 1496
# radd 17 8
# rsqm 17 8
# raddm 17 139
