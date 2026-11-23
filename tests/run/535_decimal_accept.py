# decimal 常用 API 对拍（CPython 3.14 同形脚本，逐字节 0 diff）
# NaN/Infinity 明确不支持（见 stdlib/decimal.py 模块头），不在本用例范围。
# to_integral_value 为本轮补的实现。
from decimal import Decimal, getcontext

print("init", Decimal("1.5"), Decimal(2), Decimal(0.5), Decimal("-3"))
a = Decimal("1.5")
b = Decimal("2.5")
print("add", a + b)
print("sub", a - b)
print("mul", a * b)
print("div", a / b)
print("cmp", a == b, a < b, a > b, a <= b, a >= b)
print("str", str(a), repr(a))
print("quant", Decimal("3.14159").quantize(Decimal("0.01")))
print("toint", Decimal("3.9").to_integral_value(), Decimal("-2.5").to_integral_value())
getcontext().prec = 5
print("prec", getcontext().prec, Decimal(1) / Decimal(3))

# --- expect ---
# init 1.5 2 0.5 -3
# add 4.0
# sub -1.0
# mul 3.75
# div 0.6
# cmp False True False True False
# str 1.5 Decimal('1.5')
# quant 3.14
# toint 4 -2
# prec 5 0.33333
