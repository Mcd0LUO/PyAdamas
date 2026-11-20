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

# --- expect ---
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
