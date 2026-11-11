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

# --- expect ---
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
