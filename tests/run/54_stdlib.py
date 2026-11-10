import collections
import itertools
import os
import os.path as ospath
import string
import time

print(string.capwords("hello  world"))
print(string.digits, string.hexdigits)
print(ospath.join("a", "b", "c"))
print(ospath.basename("/x/y/z.txt"), ospath.dirname("/x/y/z.txt"))
root, ext = ospath.splitext("a/b.tar.gz")
print(root, ext)
print(ospath.isdir("."), ospath.isfile("README.md"), ospath.exists("no_such_file_xyz"))
print(os.getenv("PY23_NO_SUCH_ENV_XYZ", "fallback"))
print(os.getcwd() != "")
print(time.monotonic() > 0.0, time.perf_counter() > 0.0)
time.sleep(0.001)
xs = itertools.chain([1, 2], [3, 4])
print(xs[0], xs[1], xs[2], xs[3])
rs = itertools.repeat("x", 3)
print(rs[0], rs[1], rs[2])
sl = itertools.islice([1, 2, 3, 4], 2)
print(sl[0], sl[1])
c = collections.Counter(["a", "b", "a", "c", "a"])
print(c["a"], c["b"], c.get("z", 0))
mc = c.most_common(2)
print(mc[0][0], mc[0][1], mc[1][0], mc[1][1])

# --- expect ---
# Hello World
# 0123456789 0123456789abcdefABCDEF
# a/b/c
# z.txt /x/y
# a/b.tar .gz
# True True False
# fallback
# True
# True True
# 1 2 3 4
# x x x
# 1 2
# 3 1 0
# a 3 b 1
