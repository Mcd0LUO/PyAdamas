import re
from re import search as rsearch

m = re.search(r"(\d+)-(\d+)", "abc 12-34 xyz")
if m is None:
    print("none")
else:
    print(m.group(0), m.group(1), m.group(2))
    print(m.start(0), m.end(0), m.span(0)[0], m.span(0)[1])
    gs = m.groups()
    print(gs[0], gs[1])

p = re.compile(r"ab+", re.I)
m = p.search("xxABBB")
if m is not None:
    print(m.group(0))

m = re.match("abc", "abcdef")
if m is not None:
    print(m.group(0))
m = re.fullmatch("a+", "aaa")
if m is not None:
    print(m.group(0))
print(re.fullmatch("a+", "aaab"))

for x in re.findall(r"[a-z]+", "ab 12 cd"):
    print(x)
print(re.sub(r"\d+", "#", "a1b22c"))
for x in re.split(r"\s+", "a  b\tc"):
    print(x)
print(re.escape("a.b*c d"))

m = re.search("a|ab", "ab")
if m is not None:
    print(m.group(0))
m = re.search("a+?", "aaa")
if m is not None:
    print(m.group(0))
m = re.search(r"^b", "a\nb", re.M)
if m is not None:
    print(m.group(0))
m = re.search(r"a.b", "a\nb")
print(m is None)
m = re.search(r"a.b", "a\nb", re.S)
if m is not None:
    print(m.group(0))

m = rsearch("abc", "xxabc")
if m is not None:
    print(m.group(0))

p2 = re.compile(r"\d+")
print(p2.sub("#", "a1b2"))
for x in p2.split("a1b2"):
    print(x)

print(re.sub(r"a", "X", "aaaa", count=2))
for x in re.split(r",", "a,b,c", maxsplit=1):
    print("K", x)

# --- expect ---
# 12-34 12 34
# 4 9 4 9
# 12 34
# ABBB
# abc
# aaa
# None
# ab
# cd
# a#b#c
# a
# b
# c
# a\.b\*c\ d
# a
# a
# b
# True
# a
# b
# abc
# a#b#
# a
# b
#
# XXaa
# K a
# K b,c
