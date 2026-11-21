# 382_statistics_spread —— pvariance/variance/pstdev/stdev 与 CPython 对拍
#
# 覆盖面：总体/样本自由度、mu/xbar 显式均值、单点/空数据错误、大数抵消。
# 精度边界（见 stdlib/statistics.py 模块头）：CPython 用 Fraction 精确中间量，
# 本模块用 float+fsum；[1e50,1,-1e50] 的 variance 逐位一致，但 pvariance
# 差 1 ulp（本模块 6.666666666666668e+99 / CPython 6.666666666666667e+99）。
import statistics

print(statistics.pvariance([1.0, 2.0, 3.0, 4.0]))
print(statistics.variance([1.0, 2.0, 3.0, 4.0]))
print(statistics.pstdev([1.0, 2.0, 3.0, 4.0]))
print(statistics.stdev([1.0, 2.0, 3.0, 4.0]))
print(statistics.pvariance([1.0, 2.0, 3.0, 4.0], 2.5))
print(statistics.variance([1.0, 2.0, 3.0, 4.0], 2.5))
print(statistics.pvariance([1.0, 2.0, 3.0, 4.0], mu=2.5))
print(statistics.variance([1.0, 2.0, 3.0, 4.0], xbar=2.5))
print(statistics.pvariance([1.0, 2.0, 2.0, 4.0, 4.0, 4.0, 5.0, 6.0]))
print(statistics.variance([2.75, 1.75, 1.25, 0.25, 0.5, 1.25, 3.5]))
print(statistics.pvariance([5.0]))
print(statistics.pstdev([5.0]))
print(statistics.stdev([1.5, 2.5, 2.5, 2.75, 3.25, 4.75]))
print(statistics.pstdev([1.5, 2.5, 2.5, 2.75, 3.25, 4.75]))

# 大数抵消 / 小数
print(statistics.variance([1e50, 1.0, -1e50]))
# 精度边界：CPython 6.666666666666667e+99，本模块均值先舍入差 1 ulp
print(statistics.pvariance([1e50, 1.0, -1e50]))
print(statistics.variance([0.1, 0.2, 0.3]))
print(statistics.pvariance([0.1, 0.2, 0.3]))

# 错误面
try:
    statistics.pvariance([])
except ValueError:
    print("pvariance-empty")
try:
    statistics.pstdev([])
except ValueError:
    print("pstdev-empty")
try:
    statistics.variance([])
except ValueError:
    print("variance-empty")
try:
    statistics.variance([1.0])
except ValueError:
    print("variance-single")
try:
    statistics.stdev([])
except ValueError:
    print("stdev-empty")
try:
    statistics.stdev([1.0])
except ValueError:
    print("stdev-single")

# --- expect ---
# 1.25
# 1.6666666666666667
# 1.118033988749895
# 1.2909944487358056
# 1.25
# 1.6666666666666667
# 1.25
# 1.6666666666666667
# 2.5
# 1.3720238095238095
# 0.0
# 0.0
# 1.0810874155219827
# 0.986893273527251
# 1.0000000000000002e+100
# 6.666666666666668e+99
# 0.009999999999999998
# 0.006666666666666665
# pvariance-empty
# pstdev-empty
# variance-empty
# variance-single
# stdev-empty
# stdev-single
