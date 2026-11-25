# 0.537：`del` 的保守版语义回归。
xs: list[int] = [1, 2, 3]
del xs[1]
print(xs)

d: dict[str, int] = {'a': 1, 'b': 2}
del d['a']
print(d)

y: int = 5
del y
print('after del ok')

# 重新赋值即复活
z: int = 7
del z
z = 9
print(z)

# 分支里的 del：不报错（保守版不跟踪）
w: int = 1
if w > 0:
    del w
print('branch del ok')

# --- expect ---
# [1, 3]
# {'b': 2}
# after del ok
# 9
# branch del ok
