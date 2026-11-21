"""stdlib/decimal.py + fractions.py：除零、非法输入、quantize 溢出。"""
from decimal import Decimal, getcontext
from fractions import Fraction

for bad in ["", "abc", "1.2.3", "1e", "--1", ".", "1e+", "0x10", "1e 2", "1..2"]:
    try:
        v = Decimal(bad)
        print("ok", bad, str(v))
    except Exception:
        print("bad", bad)

for den in ["0", "0.00", "-0.0"]:
    try:
        v = Decimal("1") / Decimal(den)
        print("div", den, v)
    except ZeroDivisionError:
        print("div0", den)

try:
    v = Decimal("0") / Decimal("0")
    print("zero/zero", v)
except Exception:
    print("zero/zero caught")

ctx = getcontext()
ctx.prec = 3
try:
    v = Decimal("1234.5").quantize(Decimal("1"))
    print("quant", v)
except Exception:
    print("quantize overflow")
ctx.prec = 5
print("quant ok", Decimal("123.45").quantize(Decimal("0.01")),
      Decimal("12.345").quantize(Decimal("0.01")))
ctx.prec = 28

try:
    Fraction(1, 0)
except ZeroDivisionError:
    print("fraction zero")
try:
    Fraction(12193263113702179522496570642237463801111263526900, 0)
except ZeroDivisionError:
    print("fraction zero big")
print(Fraction(12193263113702179522496570642237463801111263526900, 4))

# --- expect ---
# bad 
# bad abc
# bad 1.2.3
# bad 1e
# bad --1
# bad .
# bad 1e+
# bad 0x10
# bad 1e 2
# bad 1..2
# div0 0
# div0 0.00
# div0 -0.0
# zero/zero caught
# quantize overflow
# quant ok 123.45 12.34
# fraction zero
# fraction zero big
# 3048315778425544880624142660559365950277815881725
