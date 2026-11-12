# os / os.path 补充：makedirs / getpid / chdir / chmod / access /
# realpath / getsize / getmtime / split / samefile，以及 sep / linesep / F_OK 常量。
# 注：os.path 要用 from-import（py23 不支持 os.path.xxx 这种属性链）。
import os
from os import sep, linesep, F_OK, R_OK
from os.path import isdir, exists, getsize, getmtime, split, realpath, samefile

print(sep, linesep == "\n")
print(os.getpid() > 0)
print(os.access("/tmp", F_OK))

d = "/tmp/py23_os_test_dir"
os.makedirs(d, True)
print(isdir(d))
os.makedirs(d, True)
print(exists(d))

f = d + "/a.txt"
h = open(f, "w")
h.write("hello")
h.close()

print(getsize(f))
print(getmtime(f) > 0)
parts = split(f)
print(parts[0], parts[1])
print(realpath(f))
print(samefile(f, f))
print(samefile(f, d))

os.chmod(f, 420)
print(os.access(f, R_OK))

old = os.getcwd()
os.chdir(d)
print(os.getcwd())
os.chdir(old)

os.remove(f)
os.rmdir(d)
print(exists(d))
# --- expect ---
# / True
# True
# True
# True
# True
# 5
# True
# /tmp/py23_os_test_dir a.txt
# /tmp/py23_os_test_dir/a.txt
# True
# False
# True
# /tmp/py23_os_test_dir
# False
