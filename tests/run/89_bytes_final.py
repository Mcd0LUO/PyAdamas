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
