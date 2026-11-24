# big 的 divmod（0.488，§3.2）：与 CPython 的 (a // b, a % b) 恒等。
# 之前 check 直接报 divmod() with big is not supported；现在走 pa_big_floordiv/
# pa_big_mod，正负号、向下取整、除零都与 CPython 一致。
a: big = 1180591620717411303424
b: big = 9223372036854775808
print(divmod(a, 7))
print(divmod(-a, 7))
print(divmod(a, -7))
print(divmod(7, a))
print(divmod(b, 3))
print(divmod(a, b))
print(divmod(a, a))

# int 与 big 混用、与真除法/取模两参对照
q, r = divmod(a, 5)
print(q, r, q * 5 + r == a)
print(a // 5, a % 5)

try:
    divmod(a, 0)
except ZeroDivisionError:
    print("ZeroDivisionError")
# --- expect ---
# (168655945816773043346, 2)
# (-168655945816773043347, 5)
# (-168655945816773043347, -5)
# (0, 7)
# (3074457345618258602, 2)
# (128, 0)
# (1, 0)
# 236118324143482260684 4 True
# 236118324143482260684 4
# ZeroDivisionError
