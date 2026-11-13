"""bisect 基准：bisect_left / insort。"""
import bisect

total = 0
for _ in range(200):
    a = list(range(0, 2000, 2))
    for i in range(0, 2000, 7):
        total += bisect.bisect_left(a, i)
    for i in range(0, 500, 11):
        bisect.insort(a, i)
    total += len(a)
print(total)
