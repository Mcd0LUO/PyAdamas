"""heapq 基准：heappush / heappop。"""
import heapq

vals = [((i * 37) % 1000) for i in range(2000)]

total = 0
for _ in range(50):
    h = []
    for v in vals:
        heapq.heappush(h, v)
    for _ in range(2000):
        total += heapq.heappop(h)
print(total)
