# fractions 常用 API 对拍（CPython 3.14 同形脚本，逐字节 0 diff）
# 不支持的 from str / from float / limit_denominator 见 docs/stdlib-gaps.md。
# fractions 常用 API 对拍（CPython 3.14 同形脚本）
from fractions import Fraction

print("init", Fraction(1, 3), Fraction(2, 4), Fraction(-3, 6), Fraction(5, 1))
a = Fraction(1, 3)
b = Fraction(1, 6)
print("add", a + b)
print("sub", a - b)
print("mul", a * b)
print("div", a / b)
print("cmp", a == b, a < b, a > b, a <= b, a >= b, a == Fraction(2, 6))
print("str", str(a), repr(a))
print("nd", a.numerator, a.denominator)
print("int", Fraction(7, 7).is_integer(), a.is_integer())

# --- expect ---
# init 1/3 1/2 -1/2 5
# add 1/2
# sub 1/6
# mul 1/18
# div 2
# cmp False False True False True True
# str 1/3 Fraction(1, 3)
# nd 1 3
# int True False
