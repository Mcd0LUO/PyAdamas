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

# --- expect ---
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
