# datetime 原生类方法表对拍（CPython 3.14 同形脚本）
# date.toordinal() PA 不支持（预存缺口，见 docs/stdlib-gaps.md），不在本用例范围。
from datetime import date, datetime, timedelta

d = date(2024, 2, 29)
print("d", d.isoformat(), d.weekday())
print("dfmt", d.strftime("%Y-%m-%d"))

dt = datetime(2024, 1, 1, 12, 30, 45)
print("dt", dt.isoformat())
print("dtfmt", dt.strftime("%Y/%m/%d %H:%M:%S"))
print("dtweek", dt.weekday())

td = timedelta(days=1, seconds=3600)
print("td", td.total_seconds())
print("tddays", td.days, td.seconds)

# --- expect ---
# d 2024-02-29 3
# dfmt 2024-02-29
# dt 2024-01-01T12:30:45
# dtfmt 2024/01/01 12:30:45
# dtweek 0
# td 90000.0
# tddays 1 3600
