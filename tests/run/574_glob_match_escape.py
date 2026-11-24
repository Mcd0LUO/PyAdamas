# glob 匹配/转义口径（CPython 实跑取期望；固定小目录树）。
# 注意：pa 的 glob 返回**排序后**的 list（CPython 是 os.scandir 的目录序），
# 所以这里逐条打印前先 sorted()，只比较集合口径。
import glob
import os
import tempfile

root = tempfile.mkdtemp()
os.mkdir(os.path.join(root, "src"))
os.mkdir(os.path.join(root, "src", "x"))
os.mkdir(os.path.join(root, ".hid"))
names = ["a.txt", "b.csv", "data_1.csv", "abc.log", "src/foo.py",
         "src/x/bar.py", "src/x/baz.pyc", ".hid/h.txt"]
for n in names:
    f = open(os.path.join(root, n), "w")
    f.write("x")
    f.close()

cwd = os.getcwd()
os.chdir(root)
pats = ["*.txt", "*.csv", "data_?.csv", "[abc]*.log", "src/*.py", "src/*/*.py",
        "src/*/*.pyc", "*", "src", "nomatch*", "*.py", "src/**"]
for p in pats:
    got = sorted(glob.glob(p))
    print(p, len(got))
    print(",".join(got))
r = sorted(glob.glob("**/*.py", recursive=True))
print(len(r))
print(",".join(r))
print(len(glob.glob("")))
print(glob.escape("a*b?c[d]"), glob.escape("plain"))
os.chdir(cwd)
print(sorted(glob.glob(os.path.join(root, "*.txt")))[0].endswith("a.txt"))

# --- expect ---
# *.txt 1
# a.txt
# *.csv 2
# b.csv,data_1.csv
# data_?.csv 1
# data_1.csv
# [abc]*.log 1
# abc.log
# src/*.py 1
# src/foo.py
# src/*/*.py 1
# src/x/bar.py
# src/*/*.pyc 1
# src/x/baz.pyc
# * 5
# a.txt,abc.log,b.csv,data_1.csv,src
# src 1
# src
# nomatch* 0
# 
# *.py 0
# 
# src/** 2
# src/foo.py,src/x
# 2
# src/foo.py,src/x/bar.py
# 0
# a[*]b[?]c[[]d] plain
# True
