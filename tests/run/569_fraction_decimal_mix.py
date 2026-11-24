# Fraction / Decimal 与 int / big 的混合运算（0.486，§3.1）。
# CPython：Fraction/Decimal 的 dunder 接受 int（会提升），float 则走另一条路径
# （Fraction+float→float、Decimal+float→TypeError），本模块不覆盖 float。
from fractions import Fraction
from decimal import Decimal

f: Fraction = Fraction(1, 3)
print(f + 1, 1 + f, f - 1, 1 - f)
print(f * 3, 3 * f, f / 3, 3 / f)
print(f == Fraction(2, 6), f < 1, 1 > f, f >= 1)

# 超 i64：字面量本身是 big；另有「操作数都在 i64 内、交叉乘积越界」一例。
big1: Fraction = Fraction(1180591620717411303424, 3)
print(big1 * 3)
print(big1 + 1)
print(Fraction(1, 1180591620717411303424) * 2)
print(Fraction(3037000500, 1) * 3037000500)

# 容器里的 Fraction：不因签名放宽而变化。
xs: list[Fraction] = [Fraction(1, 2), Fraction(1, 3)]
print(xs[0] + xs[1])
d: dict[str, Fraction] = {"a": Fraction(1, 4)}
print(d["a"] * 4)

# 格式化与 str/repr。
print(str(Fraction(3, 4)), repr(Fraction(3, 4)))
print(str(Fraction(10, 2)), Fraction(10, 2).numerator)

# Decimal 侧同理。
q: Decimal = Decimal("1.1")
print(q * 3, 3 * q, q + 1, 1 + q)
print(q - 1, 2 - q, q / 2, 2 / q)
print(q == Decimal("1.10"), q < 2, 2 > q)
print(Decimal("0.1") + Decimal("0.2"))
print(str(Decimal("1.50")), repr(Decimal("1.50")))
# --- expect ---
# 4/3 4/3 -2/3 2/3
# 1 1 1/9 9
# True True True False
# 1180591620717411303424
# 1180591620717411303427/3
# 1/590295810358705651712
# 9223372037000250000
# 5/6
# 1
# 3/4 Fraction(3, 4)
# 5 5
# 3.3 3.3 2.1 2.1
# 0.1 0.9 0.55 1.818181818181818181818181818
# True True True
# 0.3
# 1.50 Decimal('1.50')
