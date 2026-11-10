import re


def upper(m):
    return m.group(0).upper()


def wrap(m):
    return "[" + m.group(0) + "]"


p = re.compile(r"[a-z]+")
print(p.sub(upper, "ab cd"))
print(p.sub(wrap, "ab cd"))
print(p.sub(upper, "ab cd", 1))

q = re.compile(r"\d+")
print(q.sub(upper, "a1b22"))
print(q.sub(upper, "abc"))

# --- expect ---
# AB CD
# [ab] [cd]
# AB cd
# a1b22
# abc
