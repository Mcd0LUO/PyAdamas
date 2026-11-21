# 383_statistics_meanq —— geometric_mean/harmonic_mean/quantiles 对拍
#
# 覆盖面：几何平均（0/负数/空、大数小数）、调和平均（0/负数/单点/空）、
# quantiles 的 inclusive/exclusive、单点数据、n<1 与未知 method。
# 精度/类型边界（见 stdlib/statistics.py 模块头）：
# - geometric_mean 与 CPython 3.14 同用 fsum(log)+exp，逐位一致。
# - harmonic_mean CPython 用精确分数求和，本模块 fsum 近似：[40,60] 得
#   47.99999999999999（CPython 48.0）；含 0 时 CPython 返回 int 0，本模块 0.0
#   （用 float() 包一层对齐打印口径）。
# - quantiles 两侧都是 double 插值同公式，逐位一致。
import statistics

print(statistics.geometric_mean([1.0, 4.0, 16.0]))
print(statistics.geometric_mean([54.0, 24.0, 36.0]))
print(statistics.geometric_mean([1.0, 0.0, 4.0]))
print(statistics.geometric_mean([2.0]))
print(statistics.geometric_mean([0.5, 2.0]))

# 精度边界：CPython 对 1/x 精确求和得 48.0，本模块 fsum 得 47.99999999999999
print(statistics.harmonic_mean([40.0, 60.0]))
print(statistics.harmonic_mean([1.0, 2.0, 3.0, 4.0]))
print(statistics.harmonic_mean([5.0]))
print(float(statistics.harmonic_mean([0.0, 2.0])))
print(float(statistics.harmonic_mean([0.0, 0.0])))
print(statistics.harmonic_mean([2.5, 5.0]))

print(statistics.quantiles([1.0, 2.0, 3.0, 4.0], n=4))
print(statistics.quantiles([1.0, 2.0, 3.0, 4.0], n=4, method="inclusive"))
print(statistics.quantiles([1.0, 2.0, 3.0, 4.0, 5.0], n=2))
print(statistics.quantiles([1.0, 2.0, 3.0, 4.0, 5.0], n=5,
                           method="inclusive"))
print(statistics.quantiles([1.0, 2.0], n=4))
print(statistics.quantiles([1.0, 2.0], n=5, method="inclusive"))
print(statistics.quantiles([7.0], n=3))
print(statistics.quantiles([1.0, 2.0, 3.0], n=1))
print(statistics.quantiles(list(range(1, 11)), n=4))
print(statistics.quantiles(list(range(1, 11)), n=10))

# 错误面
try:
    statistics.geometric_mean([])
except ValueError:
    print("geometric-empty")
try:
    statistics.geometric_mean([1.0, -1.0])
except ValueError:
    print("geometric-negative")
try:
    statistics.harmonic_mean([])
except ValueError:
    print("harmonic-empty")
try:
    statistics.harmonic_mean([1.0, -1.0])
except ValueError:
    print("harmonic-negative")
try:
    statistics.quantiles([], n=4)
except ValueError:
    print("quantiles-empty")
try:
    statistics.quantiles([1.0, 2.0], n=0)
except ValueError:
    print("quantiles-n0")
try:
    statistics.quantiles([1.0, 2.0], n=4, method="weird")
except ValueError as e:
    print("quantiles-method", type(e).__name__)

# --- expect ---
# 4.0
# 36.000000000000014
# 0.0
# 2.0
# 1.0
# 47.99999999999999
# 1.92
# 5.0
# 0.0
# 0.0
# 3.333333333333333
# [1.25, 2.5, 3.75]
# [1.75, 2.5, 3.25]
# [3.0]
# [1.8, 2.6, 3.4, 4.2]
# [0.75, 1.5, 2.25]
# [1.2, 1.4, 1.6, 1.8]
# [7.0, 7.0]
# []
# [2.75, 5.5, 8.25]
# [1.1, 2.2, 3.3, 4.4, 5.5, 6.6, 7.7, 8.8, 9.9]
# geometric-empty
# geometric-negative
# harmonic-empty
# harmonic-negative
# quantiles-empty
# quantiles-n0
# quantiles-method ValueError
