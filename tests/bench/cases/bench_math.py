"""math 基准：sqrt / sin / pow 标量循环。"""
import math

total = 0.0
for i in range(200000):
    x = float(i % 1000) * 0.001
    total += math.sqrt(x)
    total += math.sin(x)
    total += math.pow(x, 1.5)
print(total)
