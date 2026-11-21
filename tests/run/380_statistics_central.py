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
