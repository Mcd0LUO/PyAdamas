"""calendar 基准：文本月历/年历 + monthrange/weekday。"""
import calendar

total = 0
r = 0
while r < 200:
    y = 1900 + r % 200
    total += len(calendar.calendar(y))
    m = 1
    while m <= 12:
        total += len(calendar.month(y, m))
        d, n = calendar.monthrange(y, m)
        total += d + n + calendar.weekday(y, m, 15)
        m += 1
    r += 1
print(total)
