# round(big, ndigits) 与三参 pow(big, e, m)（0.495）。expect 来自 CPython 3.14.4。
b: big = 123456789012345678901234567890
print(round(b, -3))
print(round(b, -5))
print(round(b, 0))
print(round(b, 7))

x: big = 1234
print(round(x, -2), round(x, -1), round(x, 0))
print(round(15, -1), round(25, -1), round(35, -1), round(5, -1))
print(round(-15, -1), round(-25, -1), round(-35, -1))
print(round(1, -1), round(9, -1), round(10, -1))

print(pow(2, 10, 1000))
print(pow(2, 0, 7), pow(0, 0, 7), pow(5, 3, 7))
print(pow(2, 3, -5), pow(2, 3, 1))
print(pow(b, 5, 1000000007))
print(pow(2, 100, 1000000007))

c: big = 1180591620717411303424
print(pow(c, 3, 1000000007))
print(pow(3, c, 1000000007))
# --- expect ---
# 123456789012345678901234568000
# 123456789012345678901234600000
# 123456789012345678901234567890
# 123456789012345678901234567890
# 1200 1230 1234
# 20 20 40 0
# -20 -20 -40
# 0 10 10
# 24
# 1 1 6
# -2 0
# 721692643
# 976371285
# 431750151
# 892517638
