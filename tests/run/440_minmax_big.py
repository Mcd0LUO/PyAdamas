# 440: min()/max() 支持 big（比较走运行时 pa_big_cmp）。
# 裸 Cmp 的 codegen 没有 big 路径，会把 big 指针当整数比 —— 所以聚合里
# 的比较统一收口到 take_new()：big 先 BigCmp 再与 0 比较。


xs: list[big] = [1180591620717411303424, 5, 1180591620717411303425]
print(max(xs))
print(min(xs))

print(max(1180591620717411303424, 5))
print(min(5, 1180591620717411303424))
print(max(1, 2, 3))
print(min(3, 2, 1))

neg: list[big] = [-1180591620717411303424, -5]
print(max(neg))
print(min(neg))

# 普通路径不受影响
ints: list[int] = [3, 1, 2]
print(max(ints), min(ints))
floats: list[float] = [1.5, 2.5]
print(max(floats), min(floats))
strs: list[str] = ["b", "a"]
print(max(strs), min(strs))
# --- expect ---
# 1180591620717411303425
# 5
# 1180591620717411303424
# 5
# 3
# 1
# -5
# -1180591620717411303424
# 3 1
# 2.5 1.5
# b a
