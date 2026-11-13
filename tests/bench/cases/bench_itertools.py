"""itertools 基准：product / combinations / permutations / accumulate。"""
import itertools

xs8 = list(range(8))
xs12 = list(range(12))
xs7 = list(range(7))
xs200 = list(range(200))

total = 0
for _ in range(100):
    for t in itertools.product(xs8, xs8):
        total += t[0] * t[1]
    for c in itertools.combinations(xs12, 3):
        total += c[0] + c[1] + c[2]
    for p in itertools.permutations(xs7, 3):
        total += p[0]
    for x in itertools.accumulate(xs200):
        total += x
print(total)
