# 49_genexp_range —— 生成器表达式的源是 range：走边界值 + 计数循环，不再实体化（range 是 N 的 10 倍）
N = 300000
it = (x for x in range(N * 10 + 1000))
total = 0
for i in range(N):
    total += next(it)
print(total)
