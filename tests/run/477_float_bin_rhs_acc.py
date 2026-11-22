# 累加器作**右操作数**的二元运算代码生成回归（float 提升槽 + 就地运算别名坑）：
#   曾经 -O1/-O2 把 `s = a[j] - s` 发成
#       movsd a[j] -> %xmm_s ; subsd %xmm_s, %xmm_s   → 恒 0
#   （-O0 正确，因为 -O0 不做"装进目标寄存器就地算"的快速路径）。
#   根因：fast path 在装左操作数时覆盖了右操作数所在的提升槽寄存器。
#   本用例覆盖 float/int 的 +/-/*/÷、混合 `a[j]*2 + s`、n=0/1/2/3、while 形态，
#   以及左操作数就是累加器（`s = s - a[j]`，一直正确）的对照。
#   修复后要求 -O0/-O1/-O2 输出一致且与 CPython 逐字节一致。


def f_sub(a: list[float], n: int) -> float:
    s = 1.0
    for j in range(n):
        s = a[j] - s
    return s


def f_add(a: list[float], n: int) -> float:
    s = 1.0
    for j in range(n):
        s = a[j] + s
    return s


def f_mul(a: list[float], n: int) -> float:
    s = 1.5
    for j in range(n):
        s = a[j] * s
    return s


def f_div(a: list[float], n: int) -> float:
    s = 2.0
    for j in range(n):
        s = a[j] / s
    return s


def f_mixed(a: list[float], n: int) -> float:
    s = 1.0
    for j in range(n):
        s = a[j] * 2.0 + s
    return s


def f_while(a: list[float], n: int) -> float:
    s = 1.0
    j = 0
    while j < n:
        s = a[j] - s
        j = j + 1
    return s


def f_int_add(a: list[int], n: int) -> int:
    s = 1
    for j in range(n):
        s = a[j] + s
    return s


def f_int_sub(a: list[int], n: int) -> int:
    s = 1
    for j in range(n):
        s = a[j] - s
    return s


a = [0.25, 1.0, -2.25]
b = [-0.5, 2.5, -3.5]
ai = [3, -1, 5]

for n in (0, 1, 2, 3):
    print("f", n, f_sub(a, n), f_add(a, n), f_mul(a, n), f_div(a, n))
    print("g", n, f_mixed(a, n), f_while(a, n))
    print("h", n, f_int_add(ai, n), f_int_sub(ai, n))
    print("b", n, f_sub(b, n), f_add(b, n))

# 对照：累加器作左操作数（一直正确，防止本修复把它改坏）
s = 1.0
for j in range(3):
    s = s - a[j]
print("lhs", s)

# 顶层（全局槽）版本：修复前也是对的
t = 1.0
for j in range(3):
    t = a[j] - t
print("global", t)

# --- expect ---
# f 0 1.0 1.0 1.5 2.0
# g 0 1.0 1.0
# h 0 1 1
# b 0 1.0 1.0
# f 1 -0.75 1.25 0.375 0.125
# g 1 1.5 -0.75
# h 1 4 2
# b 1 -1.5 0.5
# f 2 1.75 2.25 0.375 8.0
# g 2 3.5 1.75
# h 2 3 -3
# b 2 4.0 3.0
# f 3 -4.0 0.0 -0.84375 -0.28125
# g 3 -1.0 -4.0
# h 3 8 8
# b 3 -7.5 -0.5
# lhs 2.0
# global -4.0
