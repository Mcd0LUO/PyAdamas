# 正则综合：基本匹配、API、字典取值与子表达式
# 0.381 合并自 4 个文件：56_re.py, 57_re2.py, 59_re_api.py, 61_re_dict.py

# ── 原 56_re.py ──────────────────────────────────────

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

# ── 原 57_re2.py ──────────────────────────────────────

import re

# --- 后向引用 ---
print(re.sub(r"(\w)\1", r"<\1>", "hello"))
print(re.sub(r"(\d+)-(\d+)-\1", r"[\2:\1]", "12-34-12"))
print(re.sub(r"(\w)\1", "X", "abc"))

# --- 命名组 / 命名反向引用 ---
print(re.sub(r"(?P<c>\w)(?P=c)", r"<\1>", "abbc"))
m = re.search(r"(?P<y>\d{4})-(?P<mo>\d{2})", "on 2026-11")
if m is None:
    print("NONE")
else:
    print(m.group(0), m.group("y"), m.group("mo"))

# --- lookahead ---
print(re.sub(r"\d+(?=px)", "N", "12px 34em"))
print(re.sub(r"foo(?!bar)", "X", "foobar foobaz"))

# --- lookbehind ---
print(re.sub(r"(?<=\$)\d+", "N", "42 $43"))
print(re.sub(r"(?<!\$)\b\d+", "N", "7 $42"))
m = re.search(r"(?<=\$)\d+", "42")
print(m is None)

# --- 原子组 / 占有量词 ---
print(re.sub(r"(?>a+)ab", "X", "aaab"))
print(re.sub(r"a+ab", "X", "aaab"))
print(re.sub(r"a++ab", "X", "aaab"))
print(re.sub(r"a*+b", "X", "aaab"))
print(re.sub(r"a{1,3}+ab", "X", "aaab"))

# --- 内联标志 ---
print(re.sub(r"(?i)abc", "X", "xxABC"))
print(re.sub(r"(?i:ab)c", "X", "ABc ABC"))
print(re.sub(r"(?i:ab)c", "X", "ABC"))
print(re.sub(r"(?i:ABC)def", "X", "abcdef"))

# --- \A \Z $ ---
print(re.sub(r"\Aabc", "X", "abcdef"))
print(re.sub(r"abc\Z", "X", "xabc"))
m = re.search(r"abc\Z", "abc\n")
print(m is None)
m = re.search(r"abc$", "abc\n")
print(m is None)

# --- VERBOSE ---
p = re.compile("\\d+  # int\n-\\d+  # frac\n", re.X)
print(p.sub("N", "v 12-34"))

# --- 替换模板 ---
print(re.sub(r"(\w+)@(\w+)", r"\2:\1", "user@host"))
print(re.sub(r"(?<=\d),(?=\d)", "", "1,234,567"))
print(len(re.findall(r"\d+", "a1b22")))
for x in re.findall(r"(\d)\d", "12 34"):
    print(x)
for x in re.split(r"(,)", "a,b"):
    print(x)

# ── 原 59_re_api.py ──────────────────────────────────────

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

# ── 原 61_re_dict.py ──────────────────────────────────────

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
# he<l>o
# [34:12]
# abc
# a<b>c
# 2026-11 2026 11
# Npx 34em
# foobar Xbaz
# 42 $N
# N $42
# True
# aaab
# X
# aaab
# X
# aaab
# xxX
# X ABC
# ABC
# X
# Xdef
# xX
# True
# False
# v N
# host:user
# 1234567
# 2
# 1
# 3
# a
# ,
# b
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
# 12 34
# 2 b
# 2
# only 1
# 1
# a
# 1 x
