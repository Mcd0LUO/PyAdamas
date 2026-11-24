"""copy 基准：list/dict 浅拷贝（copy.copy 两侧同名 API）。"""
import copy

base = {}
i = 0
while i < 200:
    base["k" + str(i)] = [i, i + 1, i + 2]
    i += 1
xs = list(range(1000))

total = 0
r = 0
while r < 400:
    d = copy.copy(base)
    l = copy.copy(xs)
    total += len(d) + len(l)
    r += 1
print(total)
