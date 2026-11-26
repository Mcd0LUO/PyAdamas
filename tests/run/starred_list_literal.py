# 0.565：列表字面量里的 * 展开。
a: list[int] = [1, 2]
b: list[int] = [3]
print([*a, *b])
print([0, *a, 9])
print([*a])

s: list[str] = ["x"]
print([*s, "y"])

# 原列表不被改动
print(a, b, s)

# --- expect ---
# [1, 2, 3]
# [0, 1, 2, 9]
# [1, 2]
# ['x', 'y']
# [1, 2] [3] ['x']
