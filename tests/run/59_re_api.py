import re

p = re.compile(r"(?P<a>\d+)-(\d+)")
print(p.groups)
print(p.pattern)
p2 = re.compile("a", re.I)
print((p2.flags & re.I) != 0)

m = p.search("x 12-34 y")
if m is None:
    print("NONE")
else:
    print(m.group(0), m.group(1), m.group(2))
    t = m.group(1, 2)
    print(t[0], t[1])
    print(m.expand(r"[\1|\2]"))
    print(m.expand(r"<\g<a>>"))

for mm in re.finditer(r"\d+", "a1 b22 c333"):
    print(mm.group(0), mm.start(), mm.end())

for mm in p.finditer("1-2 33-44"):
    print(mm.group(0), mm.group(2))

n = 0
for mm in re.finditer(r"\d+", "a1 b22 c333"):
    n = n + 1
print(n)
print(re.sub(r"(?P<w>\w+)@(?P<h>\w+)", r"\g<h>:\g<w>", "user@host"))

# --- expect ---
# 2
# (?P<a>\d+)-(\d+)
# True
# 12-34 12 34
# 12 34
# [12|34]
# <12>
# 1 1 2
# 22 4 6
# 333 8 11
# 1-2 2
# 33-44 44
# 3
# host:user
