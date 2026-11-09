print(int("42"), int(" -17 "), int("ff", 16), int("0x1f", 16), int("101", 2))
print(float("1.5"), float(" -2.25 "))
print(pow(2, 10, 100), pow(3, 4))
print(ord("A"), chr(66))
print(hex(255), hex(-16), bin(5), oct(8), oct(0))
print(round(2.5), round(3.5), round(-2.5), round(42), round(42, 3))
print(round(2.675, 2), round(1234, -2))
q, r = divmod(7, 2)
print(q, r)
q2, r2 = divmod(-7, 2)
print(q2, r2)
print(repr("hi"), repr(42), repr(True), repr(1.5))
print(format(42, "05d"), format(1.5, ".2f"))

# --- expect ---
# 42 -17 255 31 5
# 1.5 -2.25
# 24 81
# 65 B
# 0xff -0x10 0b101 0o10 0o0
# 2 4 -2 42 42
# 2.67 1200
# 3 1
# -4 1
# 'hi' 42 True 1.5
# 00042 1.50
