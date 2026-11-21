# 385_statistics_edges —— 可选均值哨兵/特殊值/边界口径
#
# 覆盖面：mu/xbar 显式传 0.0 时必须当"已给均值"（不能与省略的 None 混淆）、
# geometric_mean 的 inf/nan、harmonic_mean 的 nan、median_grouped 的非整数
# 组距、quantiles 的两侧插值、mode/multimode 的 0 值。
import statistics
from math import inf, nan

print(statistics.pvariance([1.0, 2.0, 3.0], 0.0))
print(statistics.variance([1.0, 2.0, 3.0], 0.0))
print(statistics.pstdev([1.0, 2.0, 3.0], 0.0))
print(statistics.stdev([1.0, 2.0, 3.0], 0.0))
print(statistics.pvariance([1.0, 2.0, 3.0], mu=0.0))

print(statistics.mean([1.0, inf]))
print(statistics.mean([1.0, -inf]))
print(statistics.mean([inf, -inf]))
print(statistics.mean([1.0, nan]))
print(statistics.fmean([1.0, inf]))
print(statistics.fmean([1.0, -inf]))
print(statistics.fmean([1.0, nan]))
try:
    statistics.fmean([inf, -inf])
except ValueError:
    print("fmean-mixed")

print(statistics.geometric_mean([1.0, inf]))
print(statistics.geometric_mean([0.0, inf]))
print(statistics.geometric_mean([1.0, nan]))
print(statistics.harmonic_mean([1.0, inf]))
print(statistics.harmonic_mean([1.0, nan]))

print(statistics.median_grouped([1.0, 2.0, 2.0, 3.0, 4.0], 0.5))
print(statistics.quantiles([1.0, 2.0, 3.0], n=2, method="inclusive"))
print(statistics.quantiles([1.0, 2.0, 3.0], n=2, method="exclusive"))
print(statistics.mode([0, 0, 1]))
print(statistics.multimode([0, 0, 1, 1]))

# --- expect ---
# 4.666666666666667
# 7.0
# 2.160246899469287
# 2.6457513110645907
# 4.666666666666667
# inf
# -inf
# nan
# nan
# inf
# -inf
# nan
# fmean-mixed
# inf
# nan
# nan
# 2.0
# nan
# 2.125
# [2.0]
# [2.0]
# 0
# [0, 1]
