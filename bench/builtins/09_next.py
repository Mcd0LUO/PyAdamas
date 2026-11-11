# 09_next —— next(生成器)：源用列表，避免把 range 实体化的开销算进来
N = 300000
src = list(range(N + 1))
it = (x for x in src)
total = 0
for i in range(N):
    total += next(it)
print(total)
