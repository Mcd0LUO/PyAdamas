# 0.640：lambda 的 keyword-only 默认值 / 参数分隔符校验
f = lambda a, *, b=2: a + b
print(f(1))

g = lambda a, b=3, *, c=4: a + b + c
print(g(1))

h = lambda a, /, b: a + b
print(h(1, 2))

# --- expect ---
# 3
# 8
# 3
