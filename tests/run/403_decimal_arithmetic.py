"""stdlib/decimal.py：定点三元组表示、精确整数内核四则、字符串形式。"""
from decimal import Decimal, getcontext

print(getcontext().prec, getcontext().rounding)

# 表示：sign/coefficient/exponent（as_tuple 的 sign 与 CPython 一样是 0/1）。
for s in ["1", "1.0", "-0", "0.00", "1E+2", "1.2300", "-12.30", "000.00", "+1.5", ".5", "5.", "1e-7"]:
    d = Decimal(s)
    print(s, "->", str(d), repr(d), d.as_tuple()[0], d.as_tuple()[1], d.as_tuple()[2])

# 四则：加减对齐指数、乘做整数乘、除做长除（精确商保留理想指数）。
print(Decimal("1.20") + Decimal("2.3"), Decimal("1.20") - Decimal("2.30"))
print(Decimal("1.20") * Decimal("2.3"), Decimal("9.99") * Decimal("9.99"))
print(Decimal("1") / Decimal("8"), Decimal("1") / Decimal("2"), Decimal("10") / Decimal("4"),
      Decimal("10.0") / Decimal("2"), Decimal("1.00") / Decimal("2"))
print(Decimal("1") / Decimal("7"), Decimal("100") / Decimal("3"), Decimal("1e10") / Decimal("3"))
# 精确商剥掉尾随零后仍超 prec：舍入位置必须按剥零后的位数算。
print(Decimal("12345678901234567890123456789") / Decimal("1"))
print(Decimal("12345678901234567890123456789") / Decimal("1.0"))
print(Decimal("12345678901234567890123456789") / Decimal("10"))
print(Decimal("999999999999999999999999999999") / Decimal("1"))
print(Decimal("1234567890123456789012345678900") / Decimal("1"))

# 精确整数内核：结果超过 i64 不回绕，超过 prec=28 才按上下文舍入。
print(Decimal("9223372036854775807") + Decimal("1"))
print(Decimal("9223372036854775807") * Decimal("2"))
print(Decimal("3037000500") * Decimal("3037000500"))
print(Decimal("123456789012345678901234567890") + Decimal("987654321098765432109876543210"))
print(Decimal("123456789012345678901234567890") * Decimal("987654321098765432109876543210"))
print(Decimal("1267650600228229401496703205376") + Decimal("0"))

# 比较 / 数值相等（1.0 == 1.00）/ 哈希契约 / dict。
a = Decimal("1.50")
b = Decimal("1.5")
print(a == b, a != b, a < b, a <= b, a > b, a >= b, a.compare(b), hash(a) == hash(b))
print(Decimal("0.00") == Decimal("-0"), Decimal("-0") < Decimal("0"),
      Decimal("1e40") > Decimal("9999999999999999999999999999999999999999"))
d: dict[Decimal, int] = {}
d[Decimal("1.0")] = 1
d[Decimal("2.50")] = 2
print(len(d), d[Decimal("1.00")], d[Decimal("2.5")])

# 查询方法。
print(Decimal("-1.20").copy_abs(), Decimal("0.00").copy_negate(),
      Decimal("-0.00").copy_negate(), Decimal("-1.20").is_signed(),
      Decimal("0.00").is_zero(), Decimal("1E+2").adjusted(), Decimal("0.001").adjusted())
print(-Decimal("1.20"), -Decimal("0.00"), abs(Decimal("-1.20")))
print(bool(Decimal("0.00")), bool(Decimal("-0.1")))
print(Decimal("0E+3"), Decimal("-0.00"), Decimal("1.5E+2"), Decimal("1.20E+2"))

# --- expect ---
# 28 ROUND_HALF_EVEN
# 1 -> 1 Decimal('1') 0 (1,) 0
# 1.0 -> 1.0 Decimal('1.0') 0 (1, 0) -1
# -0 -> -0 Decimal('-0') 1 (0,) 0
# 0.00 -> 0.00 Decimal('0.00') 0 (0,) -2
# 1E+2 -> 1E+2 Decimal('1E+2') 0 (1,) 2
# 1.2300 -> 1.2300 Decimal('1.2300') 0 (1, 2, 3, 0, 0) -4
# -12.30 -> -12.30 Decimal('-12.30') 1 (1, 2, 3, 0) -2
# 000.00 -> 0.00 Decimal('0.00') 0 (0,) -2
# +1.5 -> 1.5 Decimal('1.5') 0 (1, 5) -1
# .5 -> 0.5 Decimal('0.5') 0 (5,) -1
# 5. -> 5 Decimal('5') 0 (5,) 0
# 1e-7 -> 1E-7 Decimal('1E-7') 0 (1,) -7
# 3.50 -1.10
# 2.760 99.8001
# 0.125 0.5 2.5 5.0 0.50
# 0.1428571428571428571428571429 33.33333333333333333333333333 3333333333.333333333333333333
# 1.234567890123456789012345679E+28
# 1.234567890123456789012345679E+28
# 1234567890123456789012345679
# 1.000000000000000000000000000E+30
# 1.234567890123456789012345679E+30
# 9223372036854775808
# 18446744073709551614
# 9223372037000250000
# 1.111111110111111111011111111E+30
# 1.219326311370217952261850327E+59
# 1.267650600228229401496703205E+30
# True False False True False True 0 True
# True False True
# 2 1 2
# 1.20 -0.00 0.00 True True 2 -3
# -1.20 0.00 1.20
# False True
# 0E+3 -0.00 1.5E+2 120
