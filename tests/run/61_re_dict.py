import re

m = re.search(r"(?P<a>\d+)-(?P<b>\d+)", "x 12-34 y")
if m is None:
    print("NONE")
else:
    d = m.groupdict()
    print(d["a"], d["b"])
    print(m.lastindex, m.lastgroup)

m2 = re.search(r"(a)|(b)", "b")
if m2 is None:
    print("NONE")
else:
    print(m2.lastindex)

m3 = re.search(r"(?P<only>\d+)", "77")
if m3 is None:
    print("NONE")
else:
    print(m3.lastgroup, m3.lastindex)

m4 = re.search(r"(\w+)", "hi")
if m4 is None:
    print("NONE")
else:
    print(m4.lastindex)

p = re.compile(r"(?P<x>a)(?P<y>b)?")
m5 = p.search("a")
if m5 is None:
    print("NONE")
else:
    d2 = m5.groupdict()
    print(d2["x"])
    print(m5.lastindex, m5.lastgroup)

# --- expect ---
# 12 34
# 2 b
# 2
# only 1
# 1
# a
# 1 x
