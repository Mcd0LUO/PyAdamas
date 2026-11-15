"""stdlib/fractions.py：Fraction 的规范化与四则/比较。"""
from fractions import Fraction

a = Fraction(1, 3)
b = Fraction(1, 6)
print(a + b, a - b, a * b, a / b)
print(Fraction(6, -8), Fraction(4, 2), Fraction(4, 2).is_integer())
print(a == Fraction(2, 6), a < Fraction(1, 2), a > b)
print(Fraction(0, 1) - a)
print(repr(Fraction(1, 3)))
try:
    Fraction(1, 0)
except ZeroDivisionError:
    print("zero")

# --- expect ---
# 1/2 1/6 1/18 2
# -3/4 2 True
# True True True
# -1/3
# Fraction(1, 3)
# zero
