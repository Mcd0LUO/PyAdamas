# 标准库 logging / statistics 子集
import logging
import statistics

print(logging.getLevelName(logging.DEBUG))
print(logging.getLevelName(logging.WARNING))
print(logging.getLevelName(logging.CRITICAL))

logging.basicConfig(level=logging.INFO)
print(logging.isEnabledFor(logging.DEBUG))
print(logging.isEnabledFor(logging.INFO))

lg = logging.getLogger("app")
print(lg.name)
lg.setLevel(logging.ERROR)
print(lg.isEnabledFor(logging.WARNING))
print(lg.isEnabledFor(logging.ERROR))

xs = [1.0, 2.0, 3.0, 4.0]
print(statistics.mean(xs))
print(statistics.median(xs))
print(statistics.median_low(xs))
print(statistics.median_high(xs))
print(int(statistics.pvariance(xs) * 1000.0))
print(int(statistics.variance(xs) * 1000.0))
print(int(statistics.pstdev(xs) * 1000.0))
print(statistics.mode([3, 1, 3, 2, 3]))
print(int(statistics.harmonic_mean(xs) * 1000.0))

# --- expect ---
# DEBUG
# WARNING
# CRITICAL
# False
# True
# app
# False
# True
# 2.5
# 2.5
# 2.0
# 3.0
# 1250
# 1666
# 1118
# 3
# 1920
