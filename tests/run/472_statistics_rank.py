# statistics._rank O(n log n) 优化回归：并列秩的 Spearman 相关逐位对拍 CPython。
import statistics
xs: list[float] = [1.0, 2.0, 2.0, 3.0, 5.0, 8.0, 8.0, 9.0, 4.0, 7.0]
ys: list[float] = [2.0, 1.0, 4.0, 3.0, 9.0, 7.0, 7.0, 1.0, 5.0, 6.0]
print(statistics.correlation(xs, ys, method="ranked"))
print(statistics.correlation(ys, xs, method="ranked"))
zs: list[float] = []
i = 0
while i < len(xs):
    zs.append(xs[i] * xs[i])
    i += 1
print(statistics.correlation(xs, zs, method="ranked"))
# --- expect ---
# 0.3834355828220859
# 0.3834355828220859
# 1.0
