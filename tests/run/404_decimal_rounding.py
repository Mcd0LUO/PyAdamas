"""stdlib/decimal.py：默认 prec=28、ROUND_HALF_EVEN/ROUND_HALF_UP 等舍入边界。"""
from decimal import Decimal, getcontext, ROUND_HALF_EVEN, ROUND_HALF_UP, ROUND_DOWN, ROUND_UP

ctx = getcontext()
print(ctx.prec, ctx.rounding)

# quantize：默认走上下文舍入（HALF_EVEN，五成双），显式模式覆盖。
print(Decimal("2.5").quantize(Decimal("1")), Decimal("3.5").quantize(Decimal("1")),
      Decimal("0.5").quantize(Decimal("1")), Decimal("1.5").quantize(Decimal("1")))
print(Decimal("2.675").quantize(Decimal("0.01")), Decimal("9.99").quantize(Decimal("0.1")),
      Decimal("9.95").quantize(Decimal("0.1")), Decimal("2.60").quantize(Decimal("0.1")))
print(Decimal("-2.5").quantize(Decimal("1")), Decimal("-3.5").quantize(Decimal("1")),
      Decimal("-0.004").quantize(Decimal("0.01")))
print(Decimal("2.5").quantize(Decimal("1"), rounding=ROUND_HALF_UP),
      Decimal("-2.5").quantize(Decimal("1"), rounding=ROUND_HALF_UP),
      Decimal("2.675").quantize(Decimal("0.01"), rounding=ROUND_HALF_UP))
print(Decimal("-2.675").quantize(Decimal("0.01"), rounding=ROUND_DOWN),
      Decimal("2.671").quantize(Decimal("0.01"), rounding=ROUND_DOWN),
      Decimal("2.671").quantize(Decimal("0.01"), rounding=ROUND_UP),
      Decimal("-2.671").quantize(Decimal("0.01"), rounding=ROUND_UP))
print(Decimal("123.456").quantize(Decimal("1e2")), Decimal("150").quantize(Decimal("1e2")),
      Decimal("250").quantize(Decimal("1e2")))

# 上下文 prec：加减乘除统一按有效位数舍入。
ctx.prec = 5
print(Decimal("1") / Decimal("7"), Decimal("2") / Decimal("3"), Decimal("1") / Decimal("8"))
print(Decimal("1") + Decimal("23456"), Decimal("1") + Decimal("234567"),
      Decimal("99999") + Decimal("1"))
print(Decimal("12345") * Decimal("9"), Decimal("1.2345") * Decimal("1.0001"))
print(Decimal("1.23456") - Decimal("1.23455"), Decimal("100000") - Decimal("0.000001"))

ctx.rounding = ROUND_HALF_UP
print(Decimal("1.23455") + Decimal("0"), Decimal("1") / Decimal("8"), Decimal("1") / Decimal("6"))
ctx.rounding = ROUND_DOWN
print(Decimal("1.234567") + Decimal("0"), Decimal("1") / Decimal("6"),
      Decimal("-1.234567") + Decimal("0"))
ctx.rounding = ROUND_UP
print(Decimal("1.234561") + Decimal("0"), Decimal("1") / Decimal("6"),
      Decimal("-1.234561") + Decimal("0"))

# prec=1 的进位（999... -> 1E+n）。
ctx.prec = 1
ctx.rounding = ROUND_HALF_EVEN
print(Decimal("9.9") + Decimal("0"), Decimal("9.5") + Decimal("0"),
      Decimal("8.5") + Decimal("0"), Decimal("1") / Decimal("3"))
ctx.prec = 28
ctx.rounding = ROUND_HALF_EVEN
print(Decimal("1") / Decimal("3"))

# --- expect ---
# 28 ROUND_HALF_EVEN
# 2 4 0 2
# 2.68 10.0 10.0 2.6
# -2 -4 -0.00
# 3 -3 2.68
# -2.67 2.67 2.68 -2.68
# 1E+2 2E+2 2E+2
# 0.14286 0.66667 0.125
# 23457 2.3457E+5 1.0000E+5
# 1.1110E+5 1.2346
# 0.00001 1.0000E+5
# 1.2346 0.125 0.16667
# 1.2345 0.16666 -1.2345
# 1.2346 0.16667 -1.2346
# 1E+1 1E+1 8 0.3
# 0.3333333333333333333333333333
