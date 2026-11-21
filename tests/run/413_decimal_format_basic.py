"""stdlib/decimal.py：__format__ 基本谱（空说明/e/E/f/F/g/G/%/精度/符号/宽度/对齐/零填充）。

CPython 的 Decimal 格式化是「先按 precision 舍入再排版」，与 float 的
'g' 口径不同；舍入模式永远取 getcontext().rounding。
"""
from decimal import Decimal, getcontext, ROUND_HALF_EVEN, ROUND_HALF_UP
from decimal import ROUND_DOWN, ROUND_UP

d: Decimal = Decimal("1234.5678")

# 空说明 = 'G'（CPython 3.14 的 DefaultContext.capitals == 1），与 str() 同形
for s in ["", "G", "g"]:
    print(repr(s), repr(format(d, s)))
for v in ["0", "-0", "0.00", "0E+3", "1E+20", "1E-20", "0.000123", "-0.000123", "1.5"]:
    x: Decimal = Decimal(v)
    print(v, repr(format(x, "")), str(x) == format(x, ""))

# e / E：科学计数，指数不补零
print(format(d, "e"), format(d, "E"), format(Decimal("0.000123456"), "e"))
print(format(Decimal("-1234.5678"), "e"), format(Decimal("5"), "e"), format(Decimal("0"), "e"))
print(format(d, ".0e"), format(d, ".1e"), format(d, ".3e"), format(d, ".10e"))
# 零 + 精度：cpython 的 dotplace = 1 - precision（0.000e+3 这类怪例）
print(format(Decimal("0"), ".3e"), format(Decimal("0.00"), ".3e"),
      format(Decimal("0E+3"), ".3e"), format(Decimal("-0"), ".3e"))

# f / F：定点，不补指数
print(format(d, "f"), format(Decimal("1E+20"), "f"), format(Decimal("1E-20"), "f"))
print(format(d, ".0f"), format(d, ".2f"), format(d, ".6f"))
print(format(Decimal("0"), "f"), format(Decimal("0.00"), "f"), format(Decimal("0E+3"), "f"),
      format(Decimal("0E-5"), "f"))

# g / G：位数不超 precision 时按 f 写，否则按 e 写；指数阈值 -6
print(format(d, "g"), format(d, ".3g"), format(d, ".4g"), format(d, ".10g"))
print(format(Decimal("0.000123456"), ".3g"), format(Decimal("12345678"), ".3g"))
print(format(Decimal("1E+20"), "g"), format(Decimal("1E+20"), "g") == format(Decimal("1E+20"), ""))
print(format(Decimal("0"), ".3g"), format(Decimal("0E+3"), ".3g"), format(Decimal("0E-5"), ".3g"))

# %：乘 100 加百分号
print(format(Decimal("1.5"), "%"), format(d, "%"), format(d, ".2%"),
      format(d, ".0%"), format(Decimal("0"), "%"), format(Decimal("-0.004"), ".1%"))

# 符号：'-'（默认）/ '+' / 空格
for sg in ["", "+", " "]:
    print(repr(sg), repr(format(d, sg + ".2f")), repr(format(Decimal("-1.5"), sg + ".2f")),
          repr(format(Decimal("0"), sg + ".2f")), repr(format(Decimal("-0"), sg + ".2f")))

# 宽度 / 对齐 / 填充
print(repr(format(d, "20")), repr(format(d, ">20")), repr(format(d, "<20")), repr(format(d, "^20")))
print(repr(format(d, "*>20.2f")), repr(format(d, "*<20.2f")), repr(format(d, "-^20.2f")))
print(repr(format(d, "=+20.2f")), repr(format(d, "+20.2f")), repr(format(Decimal("-1.5"), "010.2f")))
print(repr(format(d, "3")), repr(format(d, "5.2f")), repr(format(d, "0")), repr(format(d, "00")))

# 零填充：与分组/符号/百分号组合
print(repr(format(d, "020.4f")), repr(format(Decimal("-1.5"), "012.2f")))
print(repr(format(d, "020,.2f")), repr(format(Decimal("-1234567.891"), "020,.2f")))
print(repr(format(Decimal("1.5"), "012.2%")), repr(format(Decimal("-0.5"), "010.2%")))

# 备用形式 '#'（强制小数点）与 'z'（负零不带号）
print(repr(format(Decimal("5"), "#.0f")), repr(format(Decimal("5"), ".0f")),
      repr(format(Decimal("5"), "#")), repr(format(Decimal("0.00"), "#")))
print(repr(format(Decimal("-0.00"), "z.1f")), repr(format(Decimal("-0.00"), ".1f")),
      repr(format(Decimal("-0.4"), "z.0f")), repr(format(Decimal("-0.4"), ".0f")))

# 精度与上下文舍入的交互：格式说明里的舍入跟着 getcontext().rounding 走
ctx = getcontext()
x: Decimal = Decimal("9.995")
for mode in [ROUND_HALF_EVEN, ROUND_HALF_UP, ROUND_DOWN, ROUND_UP]:
    ctx.rounding = mode
    print(mode, format(x, ".2f"), format(x, ".3g"), format(x, ".2e"), format(x, ".1%"))
ctx.rounding = ROUND_HALF_EVEN
ctx.prec = 3
print("prec3", format(d, ".2f"), format(d, ".6f"), format(d, "f"))
ctx.prec = 28

# 排版不做 prec 检查（与 quantize 不同）
print(format(Decimal("123456789012345678901234567890.5"), ".2f"))
print(format(Decimal("1E+40"), ".2e"), format(Decimal("1E-40"), ".2e"))

# --- expect ---
# '' '1234.5678'
# 'G' '1234.5678'
# 'g' '1234.5678'
# 0 '0' True
# -0 '-0' True
# 0.00 '0.00' True
# 0E+3 '0E+3' True
# 1E+20 '1E+20' True
# 1E-20 '1E-20' True
# 0.000123 '0.000123' True
# -0.000123 '-0.000123' True
# 1.5 '1.5' True
# 1.2345678e+3 1.2345678E+3 1.23456e-4
# -1.2345678e+3 5e+0 0e+0
# 1e+3 1.2e+3 1.235e+3 1.2345678000e+3
# 0.000e+3 0.000e+1 0.000e+6 -0.000e+3
# 1234.5678 100000000000000000000 0.00000000000000000001
# 1235 1234.57 1234.567800
# 0 0.00 0 0.00000
# 1234.5678 1.23e+3 1235 1234.5678
# 0.000123 1.23e+7
# 1e+20 False
# 0 0e+3 0.00000
# 150% 123456.78% 123456.78% 123457% 0% -0.4%
# '' '1234.57' '-1.50' '0.00' '-0.00'
# '+' '+1234.57' '-1.50' '+0.00' '-0.00'
# ' ' ' 1234.57' '-1.50' ' 0.00' '-0.00'
# '           1234.5678' '           1234.5678' '1234.5678           ' '     1234.5678      '
# '*************1234.57' '1234.57*************' '------1234.57-------'
# '+            1234.57' '            +1234.57' '-000001.50'
# '1234.5678' '1234.57' '1234.5678' '1234.5678'
# '000000000001234.5678' '-00000001.50'
# '0,000,000,001,234.57' '-0,000,001,234,567.89'
# '00000150.00%' '-00050.00%'
# '5.' '5' '5.' '0.00'
# '0.0' '-0.0' '0' '-0'
# ROUND_HALF_EVEN 10.00 10.0 1.00e+1 999.5%
# ROUND_HALF_UP 10.00 10.0 1.00e+1 999.5%
# ROUND_DOWN 9.99 9.99 9.99e+0 999.5%
# ROUND_UP 10.00 10.0 1.00e+1 999.5%
# prec3 1234.57 1234.567800 1234.5678
# 123456789012345678901234567890.50
# 1.00e+40 1.00e-40
