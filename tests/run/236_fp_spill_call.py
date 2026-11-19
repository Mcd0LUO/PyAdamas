# 236_fp_spill_call —— 0.314：函数内浮点累加器跨调用被踩（N19a 静默误编译）
#
# 0.313 及以前：把浮点局部变量提升到 xmm8-15 后，调用点只 spill「本轮之后
# 还会被读」的槽。gen_load_slot 对单次使用的 load 不发指令（直接把 vreg
# 别名到槽寄存器），于是 s = s + f(x) 里 load 在调用前、真正的 readsd 在
# 调用后，槽级活跃性认为调用点不必保存 —— 被调用者（自己也用 xmm8-15
# 提升）把累加器踩掉，结果静默算错。O0（整函数未提升）正确，O1/O2 错，
# 因此 0.313 的 difftest 也看不出来。
#
# 修复：别名 load 的 use 点也算槽寄存器的一次读，掩码在「use 点之前、
# 最近一次 store 之后」整段置位。本用例对拍 CPython，O0/O1/O2 全一致。

# 1) 最小形态：递归被调（不会被内联，调用必然发生）
def g(a: float, b: float) -> float:
    if a <= 0.0:
        return b
    return g(a - 1.0, b * 1.5) + 0.25

def acc(n: int) -> float:
    s: float = 0.0
    for i in range(n):
        s = s + g(float(i), 1.0)
    return s

print(acc(0))          # 0.0
print(acc(1))          # 1.0
print(acc(5))          # 15.6875
print(acc(9))          # 266.2578125

# 2) 原报形态：4×4 余子式展开（list 返回 + float 返回两个调用夹着累加）
def det3(m: list[float]) -> float:
    a: float = m[0]
    b: float = m[1]
    c: float = m[2]
    d: float = m[3]
    e: float = m[4]
    f: float = m[5]
    g3: float = m[6]
    h: float = m[7]
    i: float = m[8]
    return a * (e * i - f * h) - b * (d * i - f * g3) + c * (d * h - e * g3)

def minor(m: list[float], n: int, row: int, col: int) -> list[float]:
    out: list[float] = []
    for i in range(n):
        if i == row:
            continue
        for j in range(n):
            if j == col:
                continue
            out.append(m[i * n + j])
    return out

def det4(m: list[float]) -> float:
    s: float = 0.0
    for j in range(4):
        sign: float = 1.0 if j % 2 == 0 else -1.0
        sub: list[float] = minor(m, 4, 0, j)
        s += sign * m[j] * det3(sub)
    return s

ident: list[float] = [1.0, 0.0, 0.0, 0.0,
                      0.0, 1.0, 0.0, 0.0,
                      0.0, 0.0, 1.0, 0.0,
                      0.0, 0.0, 0.0, 1.0]
diag: list[float] = [1.0, 0.0, 0.0, 0.0,
                     0.0, 2.0, 0.0, 0.0,
                     0.0, 0.0, 3.0, 0.0,
                     0.0, 0.0, 0.0, 4.0]
m4: list[float] = [1.0, 2.0, 3.0, 4.0,
                   5.0, 6.0, 7.0, 8.0,
                   9.0, 10.0, 11.0, 12.0,
                   13.0, 14.0, 15.0, 17.0]
print(det4(ident))     # 1.0
print(det4(diag))      # 24.0
print(det4(m4))        # 0.0
print(minor(m4, 4, 0, 1))
print(det3(minor(m4, 4, 0, 1)))

# 3) 累加器在 if/else 两支里读、调用在支外：掩码必须覆盖两个块
def pick(x: float, y: float) -> float:
    if x <= 0.0:
        return y * 2.0
    return pick(x - 1.0, y + 1.0)

def acc_branchy(n: int) -> float:
    s: float = 0.0
    for i in range(n):
        t: float = pick(float(i % 3), 0.5)
        if i % 2 == 0:
            s = s + t
        else:
            s = s - t * 0.5
    return s

print(acc_branchy(6))

# 4) 同一块里两个调用，累加器只在最后使用（原报 Heisenbug 的最紧形态）
def h1(x: float) -> float:
    a: float = x * 1.5
    b: float = a + 2.25
    c: float = b * 0.75
    return c - a

def h2(x: float) -> float:
    if x >= 3.0:
        return h1(x) * 2.0
    return h1(x + 1.0) + h2(x + 1.0)

def tight(n: int) -> float:
    s: float = 0.0
    for i in range(n):
        s = s + h1(float(i)) * h2(float(i % 4))
    return s

print(tight(3))
print(tight(7))

# 5) 累加器还被写入栈（跨块出口）后继续用：store 之后的位仍要正确
def mixed(n: int) -> float:
    s: float = 0.0
    k: float = 1.0
    for i in range(n):
        s = s + k * h1(float(i))
        if s > 100.0:
            s = s / 10.0
        k = k * 1.5
    return s

print(mixed(8))

# 6) 整数累加器对照（callee-saved GPR，与本 bug 无关，钉住未被改动）
def gi(a: int, b: int) -> int:
    if a <= 0:
        return b
    return gi(a - 1, b * 3) + 1

def acc_int(n: int) -> int:
    s: int = 0
    for i in range(n):
        s = s + gi(i, 1)
    return s

print(acc_int(6))
# --- expect ---
# 0.0
# 1.0
# 15.6875
# 83.88671875
# 1.0
# 24.0
# 0.0
# [5.0, 7.0, 8.0, 9.0, 11.0, 12.0, 13.0, 15.0, 17.0]
# -8.0
# 4.5
# 11.671875
# 11.6015625
# -15.23583984375
# 379
