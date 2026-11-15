"""stdlib/statistics.py：quantiles 的 exclusive/inclusive 插值。"""
import statistics

print(statistics.quantiles([1.0, 2.0, 3.0, 4.0], n=4))
print(statistics.quantiles([1.0, 2.0, 3.0, 4.0], n=4, method="inclusive"))
print(statistics.quantiles([1.0, 2.0, 3.0, 4.0, 5.0], n=2))
print(statistics.quantiles([1.0, 2.0, 3.0, 4.0, 5.0], n=5, method="inclusive"))

try:
    statistics.quantiles([1.0], n=4)
except statistics.StatisticsError:
    print("too few")

try:
    statistics.quantiles([1.0, 2.0], n=4, method="weird")
except statistics.StatisticsError:
    print("bad method")

# --- expect ---
# [1.25, 2.5, 3.75]
# [1.75, 2.5, 3.25]
# [3.0]
# [1.8, 2.6, 3.4, 4.2]
# too few
# bad method
