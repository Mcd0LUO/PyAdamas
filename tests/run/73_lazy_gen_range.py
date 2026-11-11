# 生成器表达式的 range 源必须真惰性：以前 range 作为值会被实体化成
# 整数列表（range(10**9) 就是当场造 10 亿个元素）。
g = (x for x in range(1000000000))
print(next(g))
print(next(g))
print(next(g))

print(list(x * 2 for x in range(5)))
print(list(x for x in range(10, 0, -2)))

step = 3
print(list(x for x in range(0, 12, step)))
print(list(x for x in range(12, 0, -step)))

print(list(x for x in range(5) if x > 2))
print(list(x + y for x in range(3) for y in range(2)))
print(sum(x * x for x in range(1000)))
print(max(x for x in range(100)))

# 步长 0 是 ValueError
try:
    g0 = (x for x in range(0, 10, 0))
    print(next(g0))
except ValueError as e:
    print("ValueError", e)

# 生成器函数（不是表达式）也照旧
def gen() -> int:
    i = 0
    while i < 5:
        yield i
        i += 1

print(list(gen()))

# 动态步长的 for 循环
dst = 3
for x in range(0, 12, dst):
    print(x)
for x in range(12, 0, -dst):
    print(x)

# --- expect ---
# 0
# 1
# 2
# [0, 2, 4, 6, 8]
# [10, 8, 6, 4, 2]
# [0, 3, 6, 9]
# [12, 9, 6, 3]
# [3, 4]
# [0, 1, 1, 2, 2, 3]
# 332833500
# 99
# ValueError range() arg 3 must not be zero
# [0, 1, 2, 3, 4]
# 0
# 3
# 6
# 9
# 12
# 9
# 6
# 3
