# 字节串综合：字面量、拼接、方法与最终形态
# 0.381 合并自 5 个文件：63_bytes.py, 67_bytes_concat.py, 68_bytes_methods.py, 71_bytes_methods.py, 89_bytes_final.py

# ── 原 63_bytes.py ──────────────────────────────────────

x = b"abc"
print(x.decode())
print("hi".encode())
print(b"xyz".upper())
print(x.decode().upper())
print(len(b"abcd"))
print(b"a" + b"b" == b"ab")
print(b"abc"[0])
print(b"abc"[1:])
print(b"a" in b"abc")
print(b"q'uote")
print(b"\x00\xff")
print(b"a\nb")

# ── 原 67_bytes_concat.py ──────────────────────────────────────

# 相邻 bytes 字面量隐式拼接（CPython 语义）
x = b"ab" b"cd"
print(x)
print(len(x))
print(x == b"abcd")
print(x.decode())
print(b"a" b"b" b"c")
print(rb"r" b"w")
print(b"x" b"y".decode())
print([b"a" b"b", b"c"])
print(b"" b"x")
print(b"a\n" b"b")

# ── 原 68_bytes_methods.py ──────────────────────────────────────

# bytes 方法：操作码必须按方法名选（之前一律走成 upper）
print(b"  x  ".strip())
print(b"  x  ".lstrip())
print(b"  x  ".rstrip())
print(b"xxaxx".strip(b"x"))
print(b"AB".lower())
print(b"ab".upper())
print(b"ab".replace(b"a", b"c"))
print(b"aaa".replace(b"a", b"b", 2))
print(b"abc".replace(b"b", b""))
print(b"a".decode())
print("a".encode())

# ── 原 71_bytes_methods.py ──────────────────────────────────────

b = b"hello world"
print(b.split())
print(b.split(b"o"))
print(b.find(b"o"))
print(b.rfind(b"o"))
print(b.count(b"o"))
print(b.startswith(b"he"))
print(b.endswith(b"ld"))
print(b",".join([b"a", b"b"]))
print(b.find(b"zz"))
print(b"".join([]))

# ── 原 89_bytes_final.py ──────────────────────────────────────

# bytes 收尾：迭代产出 int、printf 风格 %、str 的 % 格式化
b = b"ABC"
total = 0
for c in b:
    total += c
print(total)

print([c for c in b"abc"])

single = b"%d" % 7
print(single)
print(b"%d-%s" % (7, b"x"))
print(b"%05d" % 42)
print(b"%x" % 255)
print(b"%s%%" % b"ok")
print(b"%c%c" % (65, b"B"))

print("%d apples" % 3)
print("%s!" % "hi")
print("%.2f" % 3.14159)
print("%05d" % -42)
print("%-5s|" % "ab")
print("%+d" % 5)
print("%.3s" % "abcdef")
print("%r" % "a")
print("%c" % 65)
print("100%%" % ())
print("%s" % (5,))

print(list(b"AB"))
print(tuple(b"AB"))
print(sum(b"AB"))
print(any(b"\x00\x01"))
print(min(b"AB"), max(b"AB"))
print(sorted(b"BA"))
print("%s %s" % (True, None))

# --- expect ---
# abc
# b'hi'
# b'XYZ'
# ABC
# 4
# True
# 97
# b'bc'
# True
# b"q'uote"
# b'\x00\xff'
# b'a\nb'
# b'abcd'
# 4
# True
# abcd
# b'abc'
# b'rw'
# xy
# [b'ab', b'c']
# b'x'
# b'a\nb'
# b'x'
# b'x  '
# b'  x'
# b'a'
# b'ab'
# b'AB'
# b'cb'
# b'bba'
# b'ac'
# a
# b'a'
# [b'hello', b'world']
# [b'hell', b' w', b'rld']
# 4
# 7
# 2
# True
# True
# b'a,b'
# -1
# b''
# 198
# [97, 98, 99]
# b'7'
# b'7-x'
# b'00042'
# b'ff'
# b'ok%'
# b'AB'
# 3 apples
# hi!
# 3.14
# -0042
# ab   |
# +5
# abc
# 'a'
# A
# 100%
# 5
# [65, 66]
# (65, 66)
# 131
# True
# 65 66
# [65, 66]
# True None
