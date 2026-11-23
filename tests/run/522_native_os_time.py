# 原生模块批次 3：os.environ / os.walk / os.system / time.localtime /
# time.gmtime / time.strftime / time.mktime。全部与 CPython 对拍（walk 排序后比较）。

import os
import time

# ── os.environ：只读快照 dict[str,str] ──────────────────────────────
print("env_nonempty", len(os.environ) > 0)
if "HOME" in os.environ:
    print("env_home_same", os.environ.get("HOME") == os.environ["HOME"])
print("env_default", os.environ.get("PYADAMAS_NO_SUCH_VAR", "dflt"))
n = 0
for k in os.environ:
    n += 1
print("env_iter_len", n == len(os.environ))
nk = 0
for k in os.environ.keys():
    nk += 1
print("env_keys_len", nk == len(os.environ))
try:
    v = os.environ["PYADAMAS_NO_SUCH_VAR"]
    print("env_missing_no_raise", v)
except KeyError:
    print("env_missing_keyerror")

# ── os.walk：立即求值（非生成器），返回 [(root, dirs, files), ...] ──
root = "/tmp/pyadamas_native_os_time"
os.makedirs(root, exist_ok=True)
os.makedirs(root + "/a/b", exist_ok=True)
h = open(root + "/f1.txt", "w")
h.write("x")
h.close()
h = open(root + "/a/f2.txt", "w")
h.write("y")
h.close()
rows = []
for r, ds, fs in list(os.walk(root)):
    ds.sort()
    fs.sort()
    rows.append((r[len(root):], ds, fs))
print("walk", rows)

# ── time：固定时间戳下字段/格式化/mktime 往返 ─────────────────────
t = time.localtime(1700000000.0)
print("local", t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8])
g = time.gmtime(1700000000.0)
print("gm", g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], g[8])
print("strftime", time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(1700000000.0)))
print("strftime2", time.strftime("%Y/%m/%d %a %b %j", time.gmtime(1700000000.0)))
print("strftime_pct", time.strftime("%%"))
print("mktime", time.mktime(time.localtime(1700000000.0)))
print("mktime_tuple", time.mktime((2023, 11, 15, 6, 13, 20, 2, 319, 0)))

# ── os.system：CPython/Unix 的 wait status ──────────────────────────
print("system_true", os.system("true"))
print("system_false", os.system("false"))

# --- expect ---
# env_nonempty True
# env_home_same True
# env_default dflt
# env_iter_len True
# env_keys_len True
# env_missing_keyerror
# walk [('', ['a'], ['f1.txt']), ('/a', ['b'], ['f2.txt']), ('/a/b', [], [])]
# local 2023 11 15 6 13 20 2 319 0
# gm 2023 11 14 22 13 20 1 318 0
# strftime 2023-11-14 22:13:20
# strftime2 2023/11/14 Tue Nov 318
# strftime_pct %
# mktime 1700000000.0
# mktime_tuple 1700000000.0
# system_true 0
# system_false 256
