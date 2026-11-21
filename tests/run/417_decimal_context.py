# 417: Decimal 上下文（localcontext / setcontext / getcontext / Context.copy /
# create_decimal）。CPython 3.14 口径：构造时 context 参数不按 prec 舍入，
# 只有 Context.create_decimal 会。
from decimal import (Decimal, Context, getcontext, setcontext, localcontext,
                     ROUND_DOWN, ROUND_HALF_UP)

print(getcontext().prec, getcontext().rounding)

c = Context(4)
print(c.create_decimal("1.23456789"))
print(c.create_decimal(123456))
print(Decimal("1.23456789", c))

w = Context(3, ROUND_HALF_UP)
cc = w.copy()
print(cc.prec, cc.rounding)

with localcontext() as lc:
    lc.prec = 5
    print(Decimal("1") / Decimal("3"))
print(getcontext().prec)

with localcontext(Context(3, ROUND_DOWN)):
    print(Decimal("123456") + Decimal("0"))
print(Decimal("123456") + Decimal("0"))

setcontext(Context(6, ROUND_DOWN))
print(Decimal("1") / Decimal("3"))
print(getcontext().prec)

# 异常路径也必须还原上下文
try:
    with localcontext(Context(2, ROUND_DOWN)):
        print(Decimal("12345") + Decimal("0"))
        raise ValueError("boom")
except ValueError:
    print("caught")
print(Decimal("12345") + Decimal("0"))
# --- expect ---
# 28 ROUND_HALF_EVEN
# 1.235
# 1.235E+5
# 1.23456789
# 3 ROUND_HALF_UP
# 0.33333
# 28
# 1.23E+5
# 123456
# 0.333333
# 6
# 1.2E+4
# caught
# 12345
