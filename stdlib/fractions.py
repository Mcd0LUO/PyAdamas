"""fractions 模块的 pyadamas 子集（纯 pyadamas 源码，随编译器分发）。

Fraction 用两个 int 字段保存规范化后的分子/分母（math.gcd 归一化，
分母恒正、零分子分母为 1），支持加减乘除、比较、取反（`0 - x`）与 str/repr。

语义边界：构造只接受 int（不支持 float/str/Decimal 构造，也不支持
Fraction 作实参）；**不定义一元负号 `__neg__`**——PyAdamas 目前对用户类
的一元负号会段错误（与 dunder 无关的编译器 bug，已单独记录），要取反请写
`Fraction(0, 1) - x`；abs()/float()/math.floor() 也不支持。
"""

import math


class Fraction:
    numerator: int
    denominator: int

    def __init__(self, numerator: int = 0, denominator: int = 1) -> None:
        if denominator == 0:
            raise ZeroDivisionError("Fraction(1, 0)")
        if denominator < 0:
            numerator = -numerator
            denominator = -denominator
        g = math.gcd(abs(numerator), denominator)
        if g > 1:
            numerator = numerator // g
            denominator = denominator // g
        self.numerator = numerator
        self.denominator = denominator

    def __add__(self, o: Fraction) -> Fraction:
        return Fraction(self.numerator * o.denominator + o.numerator * self.denominator,
                        self.denominator * o.denominator)

    def __sub__(self, o: Fraction) -> Fraction:
        return Fraction(self.numerator * o.denominator - o.numerator * self.denominator,
                        self.denominator * o.denominator)

    def __mul__(self, o: Fraction) -> Fraction:
        return Fraction(self.numerator * o.numerator,
                        self.denominator * o.denominator)

    def __truediv__(self, o: Fraction) -> Fraction:
        return Fraction(self.numerator * o.denominator,
                        self.denominator * o.numerator)

    def __eq__(self, o: Fraction) -> bool:
        return self.numerator == o.numerator and self.denominator == o.denominator

    def __lt__(self, o: Fraction) -> bool:
        return self.numerator * o.denominator < o.numerator * self.denominator

    def __le__(self, o: Fraction) -> bool:
        return self.numerator * o.denominator <= o.numerator * self.denominator

    def __gt__(self, o: Fraction) -> bool:
        return self.numerator * o.denominator > o.numerator * self.denominator

    def __ge__(self, o: Fraction) -> bool:
        return self.numerator * o.denominator >= o.numerator * self.denominator

    def __hash__(self) -> int:
        return self.numerator * 31 + self.denominator

    def __str__(self) -> str:
        if self.denominator == 1:
            return str(self.numerator)
        return str(self.numerator) + "/" + str(self.denominator)

    def __repr__(self) -> str:
        return ("Fraction(" + str(self.numerator) + ", "
                + str(self.denominator) + ")")

    def is_integer(self) -> bool:
        return self.denominator == 1
