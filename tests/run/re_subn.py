# re.subn（0.512）：返回 (结果串, 匹配次数)。与 CPython 3.14 逐字节一致。
import re

print(re.subn("a", "X", "banana"))
print(re.subn("a", "X", "banana", 1))
print(re.subn("a", "X", "banana", 2))
print(re.subn("zz", "X", "banana"))
print(re.subn("(a)(n)", r"\2\1", "banana"))
print(re.subn("", "-", "abc"))
print(re.subn("(?P<x>a)", r"[\g<x>]", "banana"))
print(re.subn("^a", "Q", "aaa", 0, re.M))
r = re.subn("a", "X", "banana")
print(type(r).__name__, len(r), r[0], r[1])

# --- expect ---
# ('bXnXnX', 3)
# ('bXnana', 1)
# ('bXnXna', 2)
# ('banana', 0)
# ('bnanaa', 2)
# ('-a-b-c-', 4)
# ('b[a]n[a]n[a]', 3)
# ('Qaa', 1)
# tuple 2 bXnXnX 3
