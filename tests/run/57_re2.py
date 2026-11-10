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

# --- expect ---
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
