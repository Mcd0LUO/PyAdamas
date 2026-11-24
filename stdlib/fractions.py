"""fractions 模块的 pyadamas 子集（纯 pyadamas 源码，随编译器分发）。

Fraction 用两个 **big**（任意精度整数）字段保存规范化后的分子/分母，
分母恒正、零分子分母为 1。数值语义与 CPython fractions.Fraction 的
精确有理数一致；规范化由本模块自带的 Euclid 最大公约数完成。

为什么不用 math.gcd：math.gcd 只接受 i64（int），在 big 上会窄化/拒收；
而约分本身必须作用在两个 big 上。因此这里用 big 的 `%`/`//`（向下取整、
余数同号）实现 Euclid，先把 a 取绝对值。

语义边界（与 CPython 的差异，均为有意）：
- 构造只接受 big/int（int 会提升成 big）；不接受 Fraction 作实参，
  也不支持 float/str/Decimal 构造。
- 四则与比较的形参是 `Fraction | int | big`（0.486 / §3.1）：int/big 走
  `_coerce` 提升成 Fraction，**float 不在签名里**（CPython 的
  `Fraction + 0.5` 返回 float，那是结果类型变化，本模块不覆盖，编译期拒绝）。
- **不定义一元负号 `__neg__`**：本轮保持公开 API 原样（0.357 的编译器
  已支持用户类一元 dunder，这是刻意的 API 边界，不是绕 bug）。要取反
  请写 `Fraction(0, 1) - x`；对 Fraction 直接写 `-x` 是编译期错误
  （bad operand type for unary -）。
- abs()/float()/math.floor() 不支持；哈希值是本模块自己的折叠
  （`cast_i64(n * 31 + d)`），与 CPython 的数值不同，但满足
  「相等对象哈希相等」的契约。
- 公开字符串形式不变：str 为 `n` 或 `n/d`，repr 为
  `Fraction(n, d)`，属性名仍为 numerator/denominator。
"""


def _gcd_big(a: big, b: big) -> big:
    """big 上的 Euclid 最大公约数（结果非负；gcd(0,0)=0）。"""
    x: big = a
    y: big = b
    if x < 0:
        x = -x
    if y < 0:
        y = -y
    while y != 0:
        t: big = x % y
        x = y
        y = t
    return x


def _coerce(o: Fraction | int | big) -> Fraction:
    """把 int / big / Fraction 归一成 Fraction（0.486 / §3.1）。

    为什么形参是「联合」而不是 unknown：四则/比较的静态签名必须能证明 `o` 有
    numerator/denominator，收 unknown 会丢掉全部检查。联合类型在运行期带标签，
    isinstance 能可靠分派；float / str / bytes 不在这三个分支里，编译期就拒绝
    （CPython 里 `Fraction + 0.5` 返回 float，那是结果类型变化，本模块不覆盖）。
    """
    if isinstance(o, Fraction):
        return o
    return Fraction(o)

class Fraction:
    numerator: big
    denominator: big

    def __init__(self, numerator: big = 0, denominator: big = 1) -> None:
        if denominator == 0:
            raise ZeroDivisionError("Fraction(1, 0)")
        if denominator < 0:
            numerator = -numerator
            denominator = -denominator
        g: big = _gcd_big(numerator, denominator)
        if g > 1:
            numerator = numerator // g
            denominator = denominator // g
        self.numerator = numerator
        self.denominator = denominator

    def __add__(self, o: Fraction | int | big) -> Fraction:
        q: Fraction = _coerce(o)
        return Fraction(self.numerator * q.denominator + q.numerator * self.denominator,
                        self.denominator * q.denominator)

    def __radd__(self, o: Fraction | int | big) -> Fraction:
        return self.__add__(o)

    def __sub__(self, o: Fraction | int | big) -> Fraction:
        q: Fraction = _coerce(o)
        return Fraction(self.numerator * q.denominator - q.numerator * self.denominator,
                        self.denominator * q.denominator)

    def __rsub__(self, o: Fraction | int | big) -> Fraction:
        q: Fraction = _coerce(o)
        return q.__sub__(self)

    def __mul__(self, o: Fraction | int | big) -> Fraction:
        q: Fraction = _coerce(o)
        return Fraction(self.numerator * q.numerator,
                        self.denominator * q.denominator)

    def __rmul__(self, o: Fraction | int | big) -> Fraction:
        return self.__mul__(o)

    def __truediv__(self, o: Fraction | int | big) -> Fraction:
        q: Fraction = _coerce(o)
        return Fraction(self.numerator * q.denominator,
                        self.denominator * q.numerator)

    def __rtruediv__(self, o: Fraction | int | big) -> Fraction:
        q: Fraction = _coerce(o)
        return q.__truediv__(self)

    def __eq__(self, o: Fraction | int | big) -> bool:
        q: Fraction = _coerce(o)
        return self.numerator == q.numerator and self.denominator == q.denominator

    def __lt__(self, o: Fraction | int | big) -> bool:
        q: Fraction = _coerce(o)
        return self.numerator * q.denominator < q.numerator * self.denominator

    def __le__(self, o: Fraction | int | big) -> bool:
        q: Fraction = _coerce(o)
        return self.numerator * q.denominator <= q.numerator * self.denominator

    def __gt__(self, o: Fraction | int | big) -> bool:
        q: Fraction = _coerce(o)
        return self.numerator * q.denominator > q.numerator * self.denominator

    def __ge__(self, o: Fraction | int | big) -> bool:
        q: Fraction = _coerce(o)
        return self.numerator * q.denominator >= q.numerator * self.denominator

    def __hash__(self) -> int:
        return cast_i64(self.numerator * 31 + self.denominator)

    def __str__(self) -> str:
        if self.denominator == 1:
            return str(self.numerator)
        return str(self.numerator) + "/" + str(self.denominator)

    def __repr__(self) -> str:
        return ("Fraction(" + str(self.numerator) + ", "
                + str(self.denominator) + ")")

    def is_integer(self) -> bool:
        return self.denominator == 1
