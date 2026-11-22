# 整数循环优化综合：边界消除、边界推导、去重
# 0.381 合并自 3 个文件：293_intloop_bce.py, 294_intloop_bce_bounds.py, 296_intloop_dedup.py

# ── 原 293_intloop_bce.py ──────────────────────────────────────

# 整数循环 BCE 回归（0.335，PYADAMAS_LOOP_OPT）：仿射 IV 下标克隆循环。
# matmul 形态（idx = 不变偏移 + IV）+ 纯 IV 下标 + 副本槽链；多种 N（含奇数、1、0）。
# 守卫通过走 unchecked 克隆；输出对拍 CPython。
def mm(a: list[int], b: list[int], c: list[int], n: int) -> None:
    for i in range(n):
        for k in range(n):
            aik = a[i * n + k]
            if aik != 0:
                for j in range(n):
                    c[i * n + j] += aik * b[k * n + j]

def mk(n: int, seed: int) -> list[int]:
    out = []
    for i in range(n * n):
        out.append((i * seed + 3) % 11)
    return out

def total(c: list[int]) -> int:
    t = 0
    for v in c:
        t += v
    return t

for n in (7, 4, 1, 0, 3):
    a = mk(n, 7)
    b = mk(n, 5)
    c = []
    for _ in range(n * n):
        c.append(0)
    mm(a, b, c, n)
    print(n, total(c))

# 纯 IV 下标 + 副本槽：s[i] = s[i] * 2 + i（存储与读取同址）
def dbl(s: list[int], n: int) -> None:
    for i in range(n):
        s[i] = s[i] * 2 + i

s = []
for i in range(9):
    s.append(i)
dbl(s, 9)
print(s)

# 偏移是运行时不变量（非常量）：base = off + j
def shift(a: list[int], off: int, n: int) -> int:
    t = 0
    for j in range(n):
        t += a[off + j]
    return t

a = []
for i in range(20):
    a.append(i * i)
print(shift(a, 5, 10))
print(shift(a, 0, 20))

# ── 原 294_intloop_bce_bounds.py ──────────────────────────────────────

# BCE 正确性命门（0.335）：守卫失败必须回退检查循环，IndexError 语义/时机不变。
# 覆盖：len 不足（中途越界，副作用已发生）、负偏移（Python 回绕）、零次循环、
# 首轮即越界、偏移溢出（max 回绕检测）、bound 为运行时变量。
def try_idx(a: list[int], n: int) -> str:
    seen = []
    try:
        for i in range(n):
            seen.append(a[i])
    except IndexError:
        return "IE after " + str(len(seen))
    return "ok " + str(len(seen))

# len=3，bound=5：第 4 轮越界，前 3 个副作用已发生
print(try_idx([10, 20, 30], 5))
# 首轮即越界（空表）
print(try_idx([], 3))
# 零次循环：不抛
print(try_idx([], 0))
print(try_idx([1], 0))

# 负偏移：idx = i - 2，Python 负下标回绕（守卫 off>=0 失败 → 原循环）
def neg_off(a: list[int], n: int) -> int:
    t = 0
    for i in range(n):
        t += a[i - 2]
    return t
print(neg_off([1, 2, 3, 4, 5], 4))

# 负常量下标直接量（affine off 为负常量）
def neg_const(a: list[int], n: int) -> int:
    t = 0
    for i in range(n):
        t += a[-1]
    return t
print(neg_const([7, 8, 9], 3))

# 偏移接近 INT64_MAX：max = (bound-1)+off 回绕 → 守卫必须失败
def huge_off(a: list[int], off: int, n: int) -> str:
    try:
        t = 0
        for j in range(n):
            t += a[off + j]
        return "ok " + str(t)
    except IndexError:
        return "IE"
print(huge_off([1, 2, 3], 9223372036854775806, 2))
print(huge_off([1, 2, 3], 9223372036854775807, 1))

# bound 是运行时变量且大于 len：中途越界
def var_bound(a: list[int], b: int) -> str:
    cnt = 0
    try:
        for i in range(b):
            cnt += a[i]
    except IndexError:
        return "IE " + str(cnt)
    return "ok " + str(cnt)
print(var_bound([5, 5, 5], 6))
print(var_bound([5, 5, 5], 3))
print(var_bound([5, 5, 5], 0))

# 存储越界时机：写坏之前先抛，表内容不变（str(list) 不在子集内，逐元素拼）
def show(a: list[int]) -> str:
    s = "["
    i = 0
    while i < 3:
        s += str(a[i])
        if i < 2:
            s += ", "
        i += 1
    return s + "]"

def store_ie(a: list[int], n: int) -> str:
    try:
        for i in range(n):
            a[i] = i * 10
    except IndexError:
        return "IE " + show(a)
    return "ok " + show(a)
print(store_ie([0, 0, 0], 5))

# 三候选循环（过摊销门槛）：守卫失败回退，IndexError 时机不变
def three(a: list[int], b: list[int], n: int) -> str:
    t = 0
    try:
        for i in range(n):
            t += a[i] + b[i] + a[i]
    except IndexError:
        return "IE " + str(t)
    return "ok " + str(t)
print(three([1, 2, 3], [10, 20, 30], 5))
print(three([1, 2, 3], [10, 20, 30], 3))
print(three([1, 2, 3], [10, 20, 30], 0))
print(three([], [], 2))

# 常量 bound >= 8（过门槛）与 < 8（跳过）：越界时机都必须不变
def const_bound(a: list[int]) -> str:
    t = 0
    try:
        for i in range(10):
            t += a[i] + a[i]
    except IndexError:
        return "IE " + str(t)
    return "ok " + str(t)
print(const_bound([1, 2, 3, 4, 5]))
print(const_bound([1, 2, 3, 4, 5, 6, 7, 8, 9, 10]))

# ── 原 296_intloop_dedup.py ──────────────────────────────────────

# 同块 load 去重回归（0.335，PYADAMAS_LOOP_OPT bit2）：LICM 提升出的重复
# load.slot / load.global 合并后语义不变（nqueens row 形态 + 全局读）。
G = 7

def f(row: int, n: int) -> int:
    t = 0
    for c in range(n):
        t += row + c
        t += row - c
        t += row * 2
    return t + G

print(f(5, 4))
print(f(0, 0))
print(f(-3, 6))

# 全局在循环里多次读（无调用循环 → LICM 提升 + 去重）
def h(n: int) -> int:
    t = 0
    for i in range(n):
        t += G
        t += G * i
    return t
print(h(5))

# 槽在循环里被写：去重不得跨写合并
def w(n: int) -> int:
    x = 1
    t = 0
    for i in range(n):
        t += x
        x = x + 1
        t += x
    return t
print(w(5))

# --- expect ---
# 7 8694
# 4 1584
# 1 9
# 0 0
# 3 681
# [0, 3, 6, 9, 12, 15, 18, 21, 24]
# 985
# 2470
# IE after 3
# IE after 0
# ok 0
# ok 0
# 12
# 27
# IE
# IE
# IE 15
# ok 15
# ok 0
# IE [0, 10, 20]
# IE 72
# ok 72
# ok 0
# IE 0
# IE 30
# ok 110
# 87
# 7
# -65
# 105
# 35
