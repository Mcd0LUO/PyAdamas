# statistics 综合：中心趋势、mode、离散度、quantiles、边界
# 0.381 合并自 6 个文件：380_statistics_central.py, 381_statistics_mode.py, 382_statistics_spread.py, 383_statistics_meanq.py, 384_statistics_two.py, 385_statistics_edges.py

# ── 原 380_statistics_central.py ──────────────────────────────────────

# 380_statistics_central —— stdlib/statistics 集中趋势与 CPython 3.14 对拍
#
# 覆盖面：mean/fmean（含 fsum 敏感大数抵消）、median/median_low/median_high、
# median_grouped（默认 interval / 显式 interval / 单点）、空数据与错误类型。
# 本模块统一 float 口径：int 数据的 mean/median 返回 float（CPython 会保留
# int），所以这里用 float 数据对拍；错误面用 try/except ValueError 验证
# StatisticsError 是 ValueError 子类。
import statistics
from statistics import StatisticsError

xs = [1.0, 2.0, 3.0, 4.0]
print(statistics.mean(xs))
print(statistics.fmean(xs))
print(statistics.mean([-1.0, 2.5, 3.25, 5.75]))
print(statistics.mean([1e50, 1.0, -1e50]))
print(statistics.mean([1.0]))
# 精度边界：CPython 精确有理数得 0.2，本模块 fsum 后除得 0.19999999999999998
print(statistics.mean([0.1, 0.2, 0.3]))
print(statistics.fmean([1e50, 1.0, -1e50]))
print(statistics.fmean([1.0]))
print(statistics.mean([1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0]))

print(statistics.median(xs))
print(statistics.median([1.0, 3.0, 5.0]))
print(statistics.median([1.0, 3.0, 5.0, 7.0]))
print(statistics.median([5.0]))
print(statistics.median_low(xs))
print(statistics.median_low([1.0, 3.0, 5.0]))
print(statistics.median_high(xs))
print(statistics.median_high([1.0, 3.0, 5.0]))

print(statistics.median_grouped([2.0, 2.0, 3.0, 3.0, 3.0, 4.0]))
print(statistics.median_grouped([1.0, 2.0, 3.0, 4.0]))
print(statistics.median_grouped([1.0, 2.0, 3.0, 4.0], 2.0))
print(statistics.median_grouped([1.0, 2.0, 3.0, 3.0, 4.0, 5.0], 10.0))
print(statistics.median_grouped([5.0]))

# 错误面：空数据
for name in ["mean", "fmean", "median", "median_low", "median_high",
             "median_grouped"]:
    try:
        if name == "mean":
            statistics.mean([])
        elif name == "fmean":
            statistics.fmean([])
        elif name == "median":
            statistics.median([])
        elif name == "median_low":
            statistics.median_low([])
        elif name == "median_high":
            statistics.median_high([])
        else:
            statistics.median_grouped([])
    except ValueError:
        print(name, "empty-error")

try:
    statistics.mode([])
except ValueError:
    print("mode empty-error")

# ── 原 381_statistics_mode.py ──────────────────────────────────────

# 381_statistics_mode —— mode/multimode 与 CPython 3.14 对拍
#
# 口径：mode 并列时返回**最先出现**的值（CPython 3.14 语义；3.8–3.10 抛
# StatisticsError 的旧行为不再沿用）；multimode 按首次出现顺序返回所有众数，
# 空数据返回 []。pa 侧只支持 list[int]（list[object] 对 float 元素的表示有
# 已知缺陷，见 stdlib/statistics.py 模块头）。
import statistics

print(statistics.mode([1, 1, 2, 3, 3, 3, 3, 4]))
print(statistics.mode([3, 1, 3, 2, 3]))
print(statistics.mode([1, 1, 2, 2]))
print(statistics.mode([5]))
print(statistics.mode([-1, -1, -2]))
print(statistics.mode([7, 7, 7]))

print(statistics.multimode([1, 1, 2, 2, 3, 3, 3]))
print(statistics.multimode([3, 3, 1, 1, 2]))
print(statistics.multimode([3, 3, 1, 1, 2, 2]))
print(statistics.multimode([1, 2, 3]))
print(statistics.multimode([5]))
print(statistics.multimode([]))
print(statistics.multimode([-2, -2, -1, -1]))

try:
    statistics.mode([])
except statistics.StatisticsError as e:
    print("mode-empty", type(e).__name__)

# ── 原 382_statistics_spread.py ──────────────────────────────────────

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

# ── 原 383_statistics_meanq.py ──────────────────────────────────────

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

# ── 原 384_statistics_two.py ──────────────────────────────────────

# 384_statistics_two —— covariance/correlation/linear_regression 对拍
#
# 覆盖面：样本协方差、Pearson/Spearman 相关（含并列秩）、线性回归
# （普通/过原点 proportional）、字段与 repr、错误面（长度不一致/点太少/
# 常量输入/未知 method）。pa 侧 linear_regression 返回 LinearRegression 类
# （slope/intercept 字段 + 同 CPython 的 repr），不是 namedtuple：不解包。
import statistics

x = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0]
y = [1.0, 2.0, 3.0, 1.0, 2.0, 3.0, 1.0, 2.0, 3.0]

print(statistics.covariance(x, y))
print(statistics.covariance([1.0, 2.0, 3.0], [1.0, 2.0, 4.0]))
print(statistics.covariance([1.0, 2.0, 3.0], [4.0, 5.0, 6.0]))
print(statistics.covariance([1.0, 2.0], [2.0, 4.0]))
print(statistics.covariance([1e50, 1.0, -1e50], [1e50, 1.0, -1e50]))

print(statistics.correlation(x, x))
print(statistics.correlation(x, y))
print(statistics.correlation([1.0, 2.0, 3.0], [1.0, 2.0, 4.0]))
print(statistics.correlation([1.0, 2.0, 2.0, 3.0],
                             [4.0, 1.0, 2.0, 3.0], method="ranked"))
print(statistics.correlation([1.0, 2.0, 3.0, 4.0],
                             [1.0, 2.0, 3.0, 4.0], method="ranked"))
print(statistics.correlation([1e50, 1.0, -1e50], [1e50, 1.0, -1e50]))

r = statistics.linear_regression(x, y)
print(r)
print(r.slope, r.intercept)
r2 = statistics.linear_regression([1.0, 2.0, 3.0], [2.0, 4.0, 6.0],
                                  proportional=True)
print(r2)
r3 = statistics.linear_regression([1.0, 2.0, 3.0], [2.0, 4.0, 5.0])
print(r3)
print(r3.slope, r3.intercept)
r4 = statistics.linear_regression([1.0, 2.0], [3.0, 5.0])
print(r4)

# 错误面
try:
    statistics.covariance([1.0], [1.0, 2.0])
except ValueError:
    print("cov-mismatch")
try:
    statistics.covariance([1.0], [2.0])
except ValueError:
    print("cov-few")
try:
    statistics.correlation([1.0], [1.0, 2.0])
except ValueError:
    print("cor-mismatch")
try:
    statistics.correlation([1.0], [2.0])
except ValueError:
    print("cor-few")
try:
    statistics.correlation([1.0, 1.0, 1.0], [1.0, 2.0, 3.0])
except ValueError as e:
    print("cor-constant", type(e).__name__)
try:
    statistics.correlation([1.0, 2.0], [1.0, 2.0], method="weird")
except ValueError as e:
    print("cor-method", type(e).__name__)
try:
    statistics.linear_regression([1.0], [1.0, 2.0])
except ValueError:
    print("lr-mismatch")
try:
    statistics.linear_regression([1.0], [2.0])
except ValueError:
    print("lr-few")
try:
    statistics.linear_regression([1.0, 1.0, 1.0], [1.0, 2.0, 3.0])
except ValueError as e:
    print("lr-constant", type(e).__name__)

# ── 原 385_statistics_edges.py ──────────────────────────────────────

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
# 2.5
# 2.5
# 2.625
# 0.3333333333333333
# 1.0
# 0.19999999999999998
# 0.3333333333333333
# 1.0
# 4.0
# 2.5
# 3.0
# 4.0
# 5.0
# 2.0
# 3.0
# 3.0
# 3.0
# 2.8333333333333335
# 2.5
# 2.0
# 3.0
# 5.0
# mean empty-error
# fmean empty-error
# median empty-error
# median_low empty-error
# median_high empty-error
# median_grouped empty-error
# mode empty-error
# 3
# 3
# 1
# 5
# -1
# 7
# [3]
# [3, 1]
# [3, 1, 2]
# [1, 2, 3]
# [5]
# []
# [-2, -1]
# mode-empty StatisticsError
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
# 0.75
# 1.5
# 1.0
# 1.0
# 1.0000000000000002e+100
# 1.0
# 0.31622776601683794
# 0.9819805060619656
# -0.31622776601683794
# 1.0
# 1.0
# LinearRegression(slope=0.1, intercept=1.5)
# 0.1 1.5
# LinearRegression(slope=2.0, intercept=0.0)
# LinearRegression(slope=1.5, intercept=0.6666666666666665)
# 1.5 0.6666666666666665
# LinearRegression(slope=2.0, intercept=1.0)
# cov-mismatch
# cov-few
# cor-mismatch
# cor-few
# cor-constant StatisticsError
# cor-method ValueError
# lr-mismatch
# lr-few
# lr-constant StatisticsError
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
