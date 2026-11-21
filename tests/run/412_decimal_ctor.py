"""stdlib/decimal.py：五种构造来源（str/int/big/float/Decimal）+ 两参形式。

float 构造口径 = 「最短 repr 再解析」，不是 CPython 3.14 的二进制精确展开：
Decimal(1.1) 是 1.1，Decimal(3.0) 是 3.0。NaN/Infinity 仍不支持但报明确
ValueError。
"""
from decimal import Decimal, getcontext

# str：原样（含尾随零与指数）
for s in ["0", "-0", "1.20", "-12.30", "1E+2", "0.00", "1e-7", "000.00"]:
    d: Decimal = Decimal(s)
    print(s, "->", str(d), repr(d))

# int / big：精确整数，不受 i64 限制
print(Decimal(0), Decimal(7), Decimal(-7), Decimal(9223372036854775807))
huge: big = 123456789012345678901234567890123456789
print(Decimal(huge))
cnt: int = 42
print(Decimal(cnt), Decimal(-cnt))

# Decimal：复制构造保留 sign/coefficient/exponent（含 -0 与正指数零）
for s in ["1.2300", "-0.00", "0E+3", "1E-1000", "123456789012345678901234567890"]:
    d = Decimal(s)
    c: Decimal = Decimal(d)
    print(s, str(c), c == d, c.as_tuple()[2] == d.as_tuple()[2])

# float：最短十进制表示（repr）再解析，不展开二进制
for f in [0.1, 1.5, -1.5, 0.0, -0.0, 3.0, 100.25]:
    print(str(Decimal(f)))

# 浮点边界与高动态范围
print(Decimal(1e16), Decimal(1e-7), Decimal(1e300), Decimal(5e-324))
print(Decimal(0.3333333333333333), Decimal(123456789.123456))
print(Decimal(1.7976931348623157e308), Decimal(2.2250738585072014e-308))
print(Decimal(1e22), Decimal(1e23))

# 与 CPython 有意不同：二进制精确展开 vs 最短 repr
print("shortest", Decimal(1.1), Decimal(0.3), Decimal(1e-10))

# 两参形式：context 只做兼容，接受后忽略（含 None 与 getcontext()）
print(Decimal("1.5", 0), Decimal("1.5", None), Decimal(3, getcontext()))
ctx = getcontext()
ctx.prec = 3
print(Decimal("12345.6789", ctx), Decimal(1.234567, ctx))
ctx.prec = 28

# unknown 静态类型也能构造（动态来源）
u: unknown = 2.5
v: unknown = "9.75"
w: unknown = 7
print(Decimal(u), Decimal(v), Decimal(w))

# 往返：Decimal(str(x)) 与 Decimal(x) 数值相等（float 来源）
f2: float = 0.1 + 0.2
print(Decimal(f2) == Decimal(str(f2)), str(Decimal(f2)))

# --- expect ---
# 0 -> 0 Decimal('0')
# -0 -> -0 Decimal('-0')
# 1.20 -> 1.20 Decimal('1.20')
# -12.30 -> -12.30 Decimal('-12.30')
# 1E+2 -> 1E+2 Decimal('1E+2')
# 0.00 -> 0.00 Decimal('0.00')
# 1e-7 -> 1E-7 Decimal('1E-7')
# 000.00 -> 0.00 Decimal('0.00')
# 0 7 -7 9223372036854775807
# 123456789012345678901234567890123456789
# 42 -42
# 1.2300 1.2300 True True
# -0.00 -0.00 True True
# 0E+3 0E+3 True True
# 1E-1000 1E-1000 True True
# 123456789012345678901234567890 123456789012345678901234567890 True True
# 0.1
# 1.5
# -1.5
# 0.0
# -0.0
# 3.0
# 100.25
# 1E+16 1E-7 1E+300 5E-324
# 0.3333333333333333 123456789.123456
# 1.7976931348623157E+308 2.2250738585072014E-308
# 1E+22 1E+23
# shortest 1.1 0.3 1E-10
# 1.5 1.5 3
# 12345.6789 1.234567
# 2.5 9.75 7
# True 0.30000000000000004
