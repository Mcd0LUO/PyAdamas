import re

pats = [r"\2(a)", r"(?P=nope)", r"a(?i)b", r"(?P<x>a)(?P<x>b)", r"a{3,1}", r"[abc", r"(?"]
for p in pats:
    try:
        re.compile(p)
        print("ok")
    except ValueError:
        print("ValueError")

print(re.sub(r"(a)?b\1", "X", "b"))
print(re.sub(r"(a)?b\1", "X", "aba"))

# --- expect ---
# ValueError
# ValueError
# ValueError
# ValueError
# ValueError
# ValueError
# ValueError
# b
# X
