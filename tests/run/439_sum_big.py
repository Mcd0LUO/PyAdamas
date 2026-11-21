# 439: sum() 支持 big 元素与 big 起始值（精确累加，不回绕）。
# 降级侧原来在累加循环里发裸 Bin Add，而 big 加法是运行时函数 —— 于是
# 即使放开类型检查也会把 big 指针当整数相加（实测段错误）。


xs: list[big] = [1180591620717411303424, 1, 2]
print(sum(xs))
print(sum(xs, 10))

empty: list[big] = []
print(sum(empty))

big_start: big = 2361183241434822606848
print(sum(xs, big_start))

# 普通整数与浮点路径不受影响
ints: list[int] = [1, 2, 3]
print(sum(ints))
floats: list[float] = [1.5, 2.5]
print(sum(floats))
# --- expect ---
# 1180591620717411303427
# 1180591620717411303437
# 0
# 3541774862152233910275
# 6
# 4.0
