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

# --- expect ---
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
