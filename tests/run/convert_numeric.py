# 数值转换的正例（P0-C）：float/int 在数值域内、str 解析、容器元素提升都不受影响。
print(float(1), float(True), float('1.5'))
print(int(3.9), int('42'), int(True))
xs: list[float] = [1.5, 2.5]
print(float(xs[0]))
ys: list[int] = [1, 2]
print(float(ys[1]))
print(int(3.9) + 1)

# --- expect ---
# 1.0 1.0 1.5
# 3 42 1
# 1.5
# 2.0
# 4
