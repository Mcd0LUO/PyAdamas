# datetime / pathlib：值类型、字段、格式化、路径属性与文件读写
import datetime
import pathlib

d = datetime.date(2024, 1, 15)
print(d.year, d.month, d.day)
print(d.isoformat())
print(d)

t = datetime.datetime(2024, 1, 15, 10, 30, 45)
print(t.year, t.month, t.day, t.hour, t.minute, t.second)
print(t.isoformat())
print(t)
print(t.strftime("%Y/%m/%d %H:%M"))
print(t.weekday())

n = datetime.now()
print(n.year > 2000)
print(len(n.isoformat()))

p = pathlib.Path("/tmp/py23_dt_dir/file.tar.gz")
print(p.name, p.stem, p.suffix)
print(p.parent)
print(p)

q = pathlib.Path("/tmp/py23_dt_write.txt")
q.write_text("hello")
print(q.exists(), q.is_file(), q.is_dir())
print(q.read_text())

r = pathlib.Path("/tmp/py23_no_such_file_xyz")
print(r.exists())
# --- expect ---
# 2024 1 15
# 2024-01-15
# 2024-01-15
# 2024 1 15 10 30 45
# 2024-01-15T10:30:45
# 2024-01-15T10:30:45
# 2024/01/15 10:30
# 0
# True
# 19
# file.tar.gz file.tar .gz
# /tmp/py23_dt_dir
# /tmp/py23_dt_dir/file.tar.gz
# True True False
# hello
# False
