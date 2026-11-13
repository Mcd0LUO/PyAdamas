from datetime import date, datetime, timedelta

t = timedelta(days=1, hours=2, minutes=30)
print(t // 2)
print(t / 2)
print(t // timedelta(hours=1))
print(t / timedelta(hours=1))
print(timedelta(seconds=-1) // 2)
print(timedelta(seconds=-1) / 2)

d = date(2024, 2, 29)
print(d.replace(year=2028))
print(d.replace(month=1, day=15))

dt = datetime(2024, 1, 31, 23, 59, 59)
print(dt.replace(hour=0, minute=0, second=0))
print(dt.replace(day=1))

# --- expect ---
# 13:15:00
# 13:15:00
# 26
# 26.5
# -1 day, 23:59:59.500000
# -1 day, 23:59:59.500000
# 2028-02-29
# 2024-01-15
# 2024-01-31T00:00:00
# 2024-01-01T23:59:59
