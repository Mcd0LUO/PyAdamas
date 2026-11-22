# 通用向量化 int64 索引算术的 32 位守卫快路径（0.418）。
#
# 向量体里 `(i+j)*(i+j+1)//2 + i + 1` 这类 i64 索引算术，在入口守卫
# （n <= K 且每个不变量 0 <= inv < K，K 由上界分析二分得出）证明所有值都
# 落在 [0, 2^31) 后，乘 → 单条 pmuludq、//2^m → 单条 psrlq、i64→f64 →
# pshufd+cvtdq2pd；守卫失败则跳标量尾（语义与标量逐位一致）。
#
# 覆盖：守卫通过的小 n、K 边界附近的极值（i+j 接近 65534 / 2^31 边界）、
# 守卫失败（大不变量 / 负数 / n > K）、与 reduction 混用、// 2 与 % 2
# 混用、无乘法的 int→float（走原路径）、Sub（可能为负，走原路径）。


def fast_small(n: int) -> int:
    total = 0.0
    for i in range(n):
        for j in range(n):
            total += 1.0 / (((i + j) * (i + j + 1) // 2) + i + 1)
    return int(total * 1000000.0)


def fast_boundary(i0: int, n: int) -> int:
    # 守卫通过（i0 < K、n <= K）时刻意把 (i+j) 顶到 65534 附近：
    # 值 = 65534*65535//2 + 32767 + 1 = 2147418113 < 2^31-1，仍在快路径范围。
    total = 0
    for j in range(n):
        total += ((i0 + j) * (i0 + j + 1) // 2) + i0 + 1
    return total


def fast_neg(i0: int, n: int) -> int:
    # 不变量为负 → 守卫失败 → 标量路径；值本身仍可用 i64 精确表示。
    total = 0
    for j in range(n):
        total += ((i0 + j) * (i0 + j + 1) // 2) + i0 + 1
    return total


def guard_fail_big(i0: int, n: int) -> int:
    # 不变量 >= K → 守卫失败 → 标量（i64 精确）。
    total = 0
    for j in range(n):
        total += ((i0 + j) * (i0 + j + 1) // 2) + i0 + 1
    return total


def guard_fail_n(n: int) -> int:
    # n > K → 守卫失败 → 标量；值本身 < 2^31（否则标量也会溢出 i64 语义）。
    total = 0
    for i in range(n):
        total += (i * (i + 1) // 2) + i + 1
    return total


def div_mod_mix(n: int) -> int:
    total = 0
    for i in range(n):
        total += ((i * i) // 2) % 2
    return total


def reduction_mix(n: int) -> int:
    v = []
    for _ in range(n):
        v.append(1.0)
    total = 0.0
    for i in range(n):
        acc = 0.0
        for j in range(n):
            acc += 1.0 / (((i + j) * (i + j + 1) // 2) + i + 1) * v[j]
        total += acc
    return int(total * 1000000.0)


def no_mul(n: int) -> int:
    # int→float 但没有乘法：不付守卫成本，走原折算路径。
    total = 0.0
    for i in range(n):
        total += float(i)
    return int(total)


def sub_expr(n: int) -> int:
    # Sub 可能为负 → 非负性不成立，不走快路径。
    total = 0
    for i in range(n):
        total += i - 3
    return total


print("fast_small", fast_small(40))
print("fast_boundary", fast_boundary(32767, 32768))
print("fast_neg", fast_neg(-10, 30))
print("guard_fail_big", guard_fail_big(1000000, 30))
print("guard_fail_n", guard_fail_n(40000))
print("div_mod_mix", div_mod_mix(100))
print("reduction_mix", reduction_mix(60))
print("no_mul", no_mul(100))
print("sub_expr", sub_expr(100))

# --- expect ---
# fast_small 7875918
# fast_boundary 41047897243648
# fast_neg 1225
# guard_fail_big 15000480004525
# guard_fail_n 10667466680000
# div_mod_mix 0
# reduction_mix 8686634
# no_mul 4950
# sub_expr 4650
