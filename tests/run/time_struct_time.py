# time.struct_time（0.518 C 部分）：localtime/gmtime/strptime 返回带 tm_* 属性的对象，
# 同时保留旧的 tuple 语义（len / 下标 / 切片 / strftime 入参）。与 CPython 3.14.4 逐字节一致。
import time

t = time.localtime(0)
print(t.tm_year, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec)
print(len(t))
print(t[0], t[-1])
print(t[:6])
print(t[:6] == (1970, 1, 1, 0, 0, 0))
g = time.gmtime(0)
print(g.tm_year, g.tm_mon, g.tm_mday)
print(time.strftime('%Y-%m-%d', t))
s = time.strptime('2020-01-01', '%Y-%m-%d')
print(s.tm_year, s.tm_mon, s.tm_mday)
print(len(s))

# --- expect ---
# 1970 1 1 8 0 0
# 9
# 1970 0
# (1970, 1, 1, 8, 0, 0)
# False
# 1970 1 1
# 1970-01-01
# 2020 1 1
# 9
