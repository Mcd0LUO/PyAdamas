"""statistics 基准：mean / median / pstdev。"""
import statistics

data = [float(i % 100) * 1.5 for i in range(1000)]

total = 0.0
for _ in range(500):
    total += statistics.mean(data)
    total += statistics.median(data)
    total += statistics.pstdev(data)
print(total)
