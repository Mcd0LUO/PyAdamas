"""stdlib/fractions.py：big 分子/分母、big 约分、跨 i64 中间乘积。

回归 CPython 3.14 fractions.Fraction 的精确有理数语义；hash 只校验
「相等对象哈希相等」的契约，不比较具体数值（两边实现不同）。
"""
from fractions import Fraction

# 归一化：分子分母都是超 i64 的 big，公因子必须是 big 上的 gcd。
a = Fraction(12193263113702179522496570642237463801111263526900, 98765432109876543210)
print(a, a.numerator, a.denominator)
print(Fraction(1267650600228229401496703205376, 633825300114114700748351602688))
print(Fraction(-12193263113702179522496570642237463801111263526900, -98765432109876543210))
print(Fraction(0, -98765432109876543210), Fraction(4, -2), Fraction(4, 2).is_integer())
print(repr(a))

# 跨 i64 中间乘积：每个字段都在 i64 内，但交叉乘积超过 2**63-1。
p = Fraction(3037000500, 1) * Fraction(3037000500, 1)
print(p, p.numerator, p.numerator > 9223372036854775807)
q = Fraction(3037000500, 1) + Fraction(1, 3037000500)
print(q, q.numerator, q.numerator > 9223372036854775807)
r = Fraction(3037000500, 1) - Fraction(1, 3037000500)
print(r, r.numerator, r.numerator > 9223372036854775807)
s = Fraction(999999937, 3037000500) / Fraction(3037000499, 1000000007)
print(s, s.numerator, s.denominator)
print(Fraction(9223372036854775807, 3) < Fraction(9223372036854775806, 3),
      Fraction(9223372036854775807, 3037000499) > Fraction(3037000500, 1),
      Fraction(1, 9223372036854775807) < Fraction(1, 9223372036854775806))

# 大数约分后四则仍与 CPython 精确有理数一致。
x = Fraction(12193263113702179522496570642237463801111263526900, 3037000500)
y = Fraction(98765432109876543210, 3037000499)
print(x + y == Fraction(x.numerator * y.denominator + y.numerator * x.denominator,
                        x.denominator * y.denominator))
print(x * y)
print(x / y)

# hash 契约：相等对象哈希相等；dict 用等值 Fraction 查得到。
d = {}
d[Fraction(1, 3)] = 1
d[Fraction(2, 6)] = 2
d[Fraction(12193263113702179522496570642237463801111263526900, 98765432109876543210)] = 9
print(hash(Fraction(2, 6)) == hash(Fraction(1, 3)), len(d),
      d[Fraction(3, 9)],
      d[Fraction(12193263113702179522496570642237463801111263526900, 98765432109876543210)])
print(Fraction(1, 3) == Fraction(2, 6), Fraction(1, 3) < Fraction(1, 2),
      Fraction(3, 1) > Fraction(2, 1))

try:
    Fraction(1, 0)
except ZeroDivisionError:
    print("zero")
try:
    Fraction(12193263113702179522496570642237463801111263526900, 0)
except ZeroDivisionError:
    print("zero big")

# --- expect ---
# 123456789012345678901234567890 123456789012345678901234567890 1
# 2
# 123456789012345678901234567890
# 0 -2 True
# Fraction(123456789012345678901234567890, 1)
# 9223372037000250000 9223372037000250000 True
# 9223372037000250001/3037000500 9223372037000250001 True
# 9223372037000249999/3037000500 9223372037000249999 True
# 999999943999999559/9223372033963249500 999999943999999559 9223372033963249500
# False True True
# True
# 6861953847602361716606975438027761653274861337457177856017805398/52554826404349
# 1388660480871968681420530201473997693/11248150
# True 2 2 9
# True True True
# zero
# zero big
