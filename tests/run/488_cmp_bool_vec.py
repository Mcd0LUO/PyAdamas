# 比较结果（bool）参与算术/归约 的回归测试（通用向量化 Cmp -> 0/1 lane）。
#
# 覆盖：int 比较 Eq/Ne 参与累加（可向量化的归约）、Lt/Le/Gt/Ge（标量路径）、
# float 比较（标量路径）、边界（相等/不等/负数/0/大数）、bool 参与加法/减法、
# bool 存进列表/作为值传递。只对拍输出。


def count_eq(flags: list, n: int) -> int:
    # 可向量化形状：count += (flags[i] == 1)
    c = 0
    i = 0
    while i < n:
        if flags[i] == 1:
            c = c + 1
        i = i + 1
    return c


def count_ne(flags: list, n: int) -> int:
    c = 0
    i = 0
    while i < n:
        if flags[i] != 0:
            c = c + 1
        i = i + 1
    return c


def cmp_bool_arith(a: list, b: list, n: int) -> int:
    # 比较结果直接当 0/1 参与算术（bool 当 int）。
    s = 0
    i = 0
    while i < n:
        s = s + (a[i] > b[i])
        s = s - (a[i] == b[i])
        i = i + 1
    return s


def order_cmps(a: list, b: list, n: int) -> int:
    # 有序比较（Lt/Le/Gt/Ge）参与计数：标量路径，输出必须正确。
    c = 0
    i = 0
    while i < n:
        if a[i] < b[i]:
            c = c + 1
        if a[i] <= b[i]:
            c = c + 2
        if a[i] > b[i]:
            c = c + 4
        if a[i] >= b[i]:
            c = c + 8
        i = i + 1
    return c


def float_cmps(a: list, b: list, n: int) -> int:
    c = 0
    i = 0
    while i < n:
        if a[i] < b[i]:
            c = c + 1
        if a[i] == b[i]:
            c = c + 2
        i = i + 1
    return c


def bool_values(n: int) -> int:
    # bool 作为值：列表元素、函数返回、加减。
    bs = []
    i = 0
    while i < n:
        bs.append(i % 2 == 0)
        i = i + 1
    s = 0
    i = 0
    while i < n:
        s = s + bs[i]
        i = i + 1
    return s


flags = []
i = 0
while i < 1000:
    if i % 3 == 0:
        flags.append(1)
    else:
        flags.append(0)
    i = i + 1
print("eq", count_eq(flags, 1000))
print("ne", count_ne(flags, 1000))

a = []
b = []
i = 0
while i < 200:
    a.append(i - 100)
    b.append((i * 7) % 13 - 6)
    i = i + 1
print("bool_arith", cmp_bool_arith(a, b, 200))
print("order", order_cmps(a, b, 200))

fa = []
fb = []
i = 0
while i < 100:
    fa.append(float(i) * 0.5 - 10.0)
    fb.append(float((i * 3) % 11) - 5.0)
    i = i + 1
print("float_cmps", float_cmps(fa, fb, 100))

# 边界：相等 / 不等 / 0 / 负数 / 大数
x = [0, 1, -1, 2147483647, 2147483648, -2147483648, 4611686018427387904]
y = [0, 1, 1, 2147483647, 2147483648, -2147483648, -4611686018427387904]
c = 0
i = 0
while i < 7:
    if x[i] == y[i]:
        c = c + 1
    if x[i] != y[i]:
        c = c + 10
    i = i + 1
print("bounds", c)
print("bools", bool_values(100))

# --- expect ---
# eq 334
# ne 334
# bool_arith 98
# order 1498
# float_cmps 22
# bounds 25
# bools 50
