"""datetime 基准：timedelta 算术、字段读取、日期差。"""
from datetime import datetime, timedelta

base = datetime(2024, 1, 1, 0, 0, 0)
step = timedelta(hours=1, minutes=30)

total = 0
for i in range(20000):
    cur = base + step * (i % 100)
    total += cur.day + cur.hour
    diff = cur - base
    total += diff.days + diff.seconds
print(total)
