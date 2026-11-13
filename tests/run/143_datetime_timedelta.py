from datetime import date, datetime, timedelta

d1 = date(2024, 1, 31)
d2 = date(2024, 2, 29)
delta = d2 - d1
print(delta.days, delta.seconds, delta.microseconds)
print(delta)
print(d1 + timedelta(days=29))
print(d2 - timedelta(days=1))
print(d1 < d2, d1 == d2, d1 == date(2024, 1, 31))

dt1 = datetime(2024, 1, 1, 10, 30, 0)
dt2 = datetime(2024, 1, 2, 11, 45, 30)
diff = dt2 - dt1
print(diff.days, diff.seconds)
print(diff)
print(diff.total_seconds())
print(dt1 + timedelta(hours=1, minutes=30))
print(dt2 - timedelta(days=1, seconds=30))
print(dt1 == datetime(2024, 1, 1, 10, 30, 0), dt1 < dt2)

t = timedelta(days=1, hours=2, minutes=3, seconds=4, milliseconds=5)
print(t.days, t.seconds, t.microseconds)
print(t)
print(timedelta(seconds=3661))
print(timedelta(seconds=-1))
print(-timedelta(seconds=5))
print(timedelta(seconds=5) * 3)
print(3 * timedelta(seconds=5))
print(timedelta(seconds=5) + timedelta(seconds=6))
print(timedelta(seconds=5) - timedelta(seconds=6))
print(timedelta(seconds=5) < timedelta(seconds=6))
print(timedelta(microseconds=1500000))
print(timedelta(weeks=1, days=1))

# --- expect ---
# 29 0 0
# 29 days, 0:00:00
# 2024-02-29
# 2024-02-28
# True False True
# 1 4530
# 1 day, 1:15:30
# 90930.0
# 2024-01-01T12:00:00
# 2024-01-01T11:45:00
# True True
# 1 7384 5000
# 1 day, 2:03:04.005000
# 1:01:01
# -1 day, 23:59:59
# -1 day, 23:59:55
# 0:00:15
# 0:00:15
# 0:00:11
# -1 day, 23:59:59
# True
# 0:00:01.500000
# 8 days, 0:00:00
