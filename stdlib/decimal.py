"""decimal 模块的 pyadamas 子集：定点十进制，精确整数内核。

## 表示

Decimal 是三元组 `(sign, coefficient, exponent)`：

    value = (-1) ** sign * coefficient * 10 ** exponent

- `sign`：0 或 1（1 = 负，包含负零；与 CPython as_tuple() 的 sign 同一口径）。
- `coefficient`：非负 **big**（任意精度整数），不带前导零；零的 coefficient 为 0。
- `exponent`：int（i64）十进制指数。

尾随零是有意义的表示：Decimal("1.20") 的 coefficient=120、exponent=-2，
与 Decimal("1.2") 数值相等但字符串不同（同 CPython）。所有运算都先在
big 上做精确整数运算，再按上下文精度舍入，不存在 i64 回绕。

## 默认上下文与精度

- 模块级上下文由 getcontext() 返回，字段 `prec`（有效数字位数，默认 **28**）
  与 `rounding`（默认 **ROUND_HALF_EVEN**）。可以直接改
  `getcontext().prec = 10`，对之后的所有运算生效。
- 支持的舍入模式（常量是字符串，与 CPython 同名同值）：
  ROUND_HALF_EVEN（默认，四舍六入五成双）、ROUND_HALF_UP（五入）、
  ROUND_DOWN（向零截断）、ROUND_UP（远离零进位）。
- 四则（+ - * /）与 CPython 一样按 prec 舍入：加减先对齐指数做精确
  整数加减；乘做整数乘、指数相加；除做整数长除，正确舍入到 prec 位，
  精确商按 CPython 的「理想指数」保留尾随零（10.0/2 == 5.0，
  1.00/2 == 0.50，10/4 == 2.5）。
- quantize(exp, rounding="") 按给定指数定点舍入，指数来自参数 Decimal
  的 exponent（Decimal("0.01") → -2，Decimal("1e2") → 2）。

## 明确不做的 CPython 行为（不假装完整兼容）

- 没有上下文管理器 localcontext()、没有线程本地上下文、没有 setcontext()。
- 没有 traps / signals / flags / Emin / Emax / Subnormal / Underflow：
  不做指数范围检查，不设置 Inexact/Rounded 之类的上下文标志。
- 没有 NaN、sNaN、Infinity、-Infinity；字符串里出现这些直接 ValueError。
- 构造只接受 **str**：不支持 Decimal(int/float/big/Decimal) 构造，
  也没有 from_float()；需要从整数来请写 Decimal(str(n))。
- 不支持与 int/float/big 的混合运算或比较（先显式转成 Decimal 字符串），
  也不支持 `**`、`//`、`%`、`+`/`-` 一元正号、round()、math 函数。
- 不实现 `__format__`（f-string 的小数格式），`as_tuple()` 返回普通 tuple
  而不是 DecimalTuple 具名元组，`compare()` 返回 int -1/0/1 而不是 Decimal。
- 异常层次不建模：除零抛 ZeroDivisionError；非法字面量/非法运算
  （如 quantize 结果超出 prec）抛 ValueError，没有 decimal.DecimalException
  子类。
"""

# 舍入模式常量：与 CPython 一样是字符串，可直接比较/传参。
ROUND_HALF_EVEN: str = "ROUND_HALF_EVEN"
ROUND_HALF_UP: str = "ROUND_HALF_UP"
ROUND_DOWN: str = "ROUND_DOWN"
ROUND_UP: str = "ROUND_UP"


class Context:
    """十进制上下文：只保留 prec 与 rounding 两个字段。"""

    prec: int
    rounding: str

    def __init__(self, prec: int = 28,
                 rounding: str = "ROUND_HALF_EVEN") -> None:
        self.prec = prec
        self.rounding = rounding


_context: Context = Context()


def getcontext() -> Context:
    """返回模块级上下文（默认 prec=28, ROUND_HALF_EVEN）。"""
    return _context


# ── big 上的整数工具 ──────────────────────────────────────────────


def _pow10(k: int) -> big:
    """10 ** k，k 非负。"""
    p: big = 1
    i: int = 0
    while i < k:
        p = p * 10
        i = i + 1
    return p


def _ndigits(c: big) -> int:
    """非负 big 的十进制位数（0 记为 1 位）。"""
    if c == 0:
        return 1
    return len(str(c))


def _digits_list(c: big) -> list[int]:
    """非负 big 的十进制数字列表；0 → [0]。"""
    out: list[int] = []
    v: big = c
    if v == 0:
        out.append(0)
        return out
    while v > 0:
        out.append(cast_i64(v % 10))
        v = v // 10
    out.reverse()
    return out


def _round_mag(q: big, drop: int, mode: str, sticky: bool) -> big:
    """把非负整数 q 丢弃低 drop 位，按 mode 对「被丢弃部分」舍入。

    sticky 表示被丢弃部分之后还有非零尾巴（长除的余数非零），
    用于把「正好一半」判成「大于一半」。
    """
    if drop <= 0:
        return q
    p: big = _pow10(drop)
    half: big = p // 2
    lo: big = q % p
    hi: big = q // p
    inc: bool = False
    if mode == ROUND_HALF_UP:
        if lo >= half:
            inc = True
    elif mode == ROUND_HALF_EVEN:
        if lo > half:
            inc = True
        elif lo == half:
            if sticky:
                inc = True
            elif hi % 2 == 1:
                inc = True
    elif mode == ROUND_UP:
        if lo != 0 or sticky:
            inc = True
    # ROUND_DOWN（以及未知模式）向零截断
    if inc:
        hi = hi + 1
    return hi


def _round_parts(sign: int, coeff: big, exp: int, prec: int,
                 mode: str) -> tuple[int, big, int]:
    """按有效数字位数 prec 对 (sign, coeff, exp) 舍入。"""
    n: int = _ndigits(coeff)
    if n <= prec:
        return sign, coeff, exp
    drop: int = n - prec
    q: big = _round_mag(coeff, drop, mode, False)
    exp = exp + drop
    if _ndigits(q) > prec:
        q = q // 10
        exp = exp + 1
    return sign, q, exp


def _addsub_parts(sa: int, ca: big, ea: int, sb: int, cb: big, eb: int,
                  subtract: bool, prec: int, mode: str) -> tuple[int, big, int]:
    """加/减的精确整数内核 + 上下文舍入。"""
    sb_eff: int = sb
    if subtract:
        sb_eff = 1 - sb
    e: int = ea
    if eb < e:
        e = eb
    xa: big = ca * _pow10(ea - e)
    xb: big = cb * _pow10(eb - e)
    na: big = xa
    if sa == 1:
        na = 0 - xa
    nb: big = xb
    if sb_eff == 1:
        nb = 0 - xb
    n: big = na + nb
    sign: int = 0
    if n < 0:
        sign = 1
        n = 0 - n
    elif n == 0:
        # 零结果的符号位：两个有效操作数都负才是 -0（decimal 规范）。
        sign = sa & sb_eff
    return _round_parts(sign, n, e, prec, mode)


def _div_parts(sa: int, ca: big, ea: int, sb: int, cb: big, eb: int,
               prec: int, mode: str) -> tuple[int, big, int]:
    """真除的精确整数内核：长除 + 正确舍入到 prec 位有效数字。"""
    if cb == 0:
        raise ZeroDivisionError("decimal division by zero")
    sign: int = sa ^ sb
    ideal: int = ea - eb
    if ca == 0:
        zero: big = 0
        return sign, zero, ideal
    da: int = _ndigits(ca)
    db: int = _ndigits(cb)
    k: int = prec + 1 + db - da
    if k < 0:
        k = 0
    q: big = 0
    r: big = 1
    while True:
        num: big = ca * _pow10(k)
        q = num // cb
        r = num % cb
        if r == 0:
            break
        if _ndigits(q) >= prec + 1:
            break
        k = k + 1
    nq: int = _ndigits(q)
    if r == 0:
        # 精确商：按 CPython 的理想指数保留尾随零（不把指数抬到 ideal 之上）。
        exp: int = ideal - k
        while exp < ideal and q % 10 == 0:
            q = q // 10
            exp = exp + 1
        if _ndigits(q) <= prec:
            return sign, q, exp
    drop: int = nq - prec
    if drop < 0:
        drop = 0
    q2: big = _round_mag(q, drop, mode, r != 0)
    exp2: int = ideal - k + drop
    if _ndigits(q2) > prec:
        q2 = q2 // 10
        exp2 = exp2 + 1
    return sign, q2, exp2


def _from_parts(sign: int, coeff: big, exp: int) -> Decimal:
    """内部构造：直接写入 sign/coefficient/exponent。"""
    d: Decimal = Decimal("0")
    d._sign = sign
    d._coeff = coeff
    d._exp = exp
    return d


def _exp_text(e: int) -> str:
    if e < 0:
        return "-" + str(0 - e)
    return "+" + str(e)


def _cmp_parts(sa: int, ca: big, ea: int, sb: int, cb: big,
               eb: int) -> int:
    """数值比较（忽略零的符号位），返回 -1/0/1。"""
    za: int = 0
    if ca == 0:
        za = 1
    zb: int = 0
    if cb == 0:
        zb = 1
    if za == 1 and zb == 1:
        return 0
    if za == 1:
        if sb == 1:
            return 1
        return -1
    if zb == 1:
        if sa == 1:
            return -1
        return 1
    if sa != sb:
        if sa == 1:
            return -1
        return 1
    xa: big = ca
    xb: big = cb
    if ea > eb:
        xa = xa * _pow10(ea - eb)
    elif eb > ea:
        xb = xb * _pow10(eb - ea)
    m: int = 0
    if xa < xb:
        m = -1
    elif xa > xb:
        m = 1
    if sa == 1:
        m = 0 - m
    return m


class Decimal:
    """定点十进制数（见模块 docstring 的表示与边界）。"""

    _sign: int
    _coeff: big
    _exp: int

    def __init__(self, value: str = "0") -> None:
        self._sign = 0
        self._coeff = 0
        self._exp = 0
        self._parse(value)

    def _parse(self, s: str) -> None:
        n: int = len(s)
        if n == 0:
            raise ValueError("Decimal(): empty string")
        i: int = 0
        sign: int = 0
        c: str = s[0]
        if c == "-" or c == "+":
            if c == "-":
                sign = 1
            i = 1
        int_start: int = i
        while i < n and s[i] >= "0" and s[i] <= "9":
            i = i + 1
        int_end: int = i
        frac_start: int = i
        frac_end: int = i
        if i < n and s[i] == ".":
            i = i + 1
            frac_start = i
            while i < n and s[i] >= "0" and s[i] <= "9":
                i = i + 1
            frac_end = i
        if int_end == int_start and frac_end == frac_start:
            raise ValueError("Decimal(): invalid literal")
        exp_val: int = 0
        if i < n and (s[i] == "e" or s[i] == "E"):
            i = i + 1
            esign: int = 1
            if i < n and (s[i] == "+" or s[i] == "-"):
                if s[i] == "-":
                    esign = -1
                i = i + 1
            estart: int = i
            while i < n and s[i] >= "0" and s[i] <= "9":
                i = i + 1
            if i == estart:
                raise ValueError("Decimal(): invalid exponent")
            exp_val = int(s[estart:i]) * esign
        if i != n:
            raise ValueError("Decimal(): invalid literal")
        coeff: big = 0
        j: int = int_start
        while j < int_end:
            coeff = coeff * 10 + (ord(s[j]) - 48)
            j = j + 1
        j = frac_start
        while j < frac_end:
            coeff = coeff * 10 + (ord(s[j]) - 48)
            j = j + 1
        frac_len: int = frac_end - frac_start
        self._sign = sign
        self._coeff = coeff
        self._exp = exp_val - frac_len

    # ── 四则 ──────────────────────────────────────────────────

    def __add__(self, o: Decimal) -> Decimal:
        sign, coeff, exp = _addsub_parts(self._sign, self._coeff, self._exp,
                                         o._sign, o._coeff, o._exp, False,
                                         _context.prec, _context.rounding)
        return _from_parts(sign, coeff, exp)

    def __sub__(self, o: Decimal) -> Decimal:
        sign, coeff, exp = _addsub_parts(self._sign, self._coeff, self._exp,
                                         o._sign, o._coeff, o._exp, True,
                                         _context.prec, _context.rounding)
        return _from_parts(sign, coeff, exp)

    def __mul__(self, o: Decimal) -> Decimal:
        sign: int = self._sign ^ o._sign
        coeff: big = self._coeff * o._coeff
        exp: int = self._exp + o._exp
        sign, coeff, exp = _round_parts(sign, coeff, exp, _context.prec,
                                        _context.rounding)
        return _from_parts(sign, coeff, exp)

    def __truediv__(self, o: Decimal) -> Decimal:
        sign, coeff, exp = _div_parts(self._sign, self._coeff, self._exp,
                                      o._sign, o._coeff, o._exp,
                                      _context.prec, _context.rounding)
        return _from_parts(sign, coeff, exp)

    # ── 一元运算（不做 __pos__：CPython 的 +x 也要上下文舍入）────

    def __neg__(self) -> Decimal:
        # CPython 的一元负号等价于 0 - x：零结果恒为正零。
        if self._coeff == 0:
            return _from_parts(0, 0, self._exp)
        return _from_parts(1 - self._sign, self._coeff, self._exp)

    def __abs__(self) -> Decimal:
        if self._coeff == 0:
            return _from_parts(0, 0, self._exp)
        return _from_parts(0, self._coeff, self._exp)

    def __bool__(self) -> bool:
        return self._coeff != 0

    # ── 比较 / 哈希 ───────────────────────────────────────────

    def __eq__(self, o: Decimal) -> bool:
        return _cmp_parts(self._sign, self._coeff, self._exp,
                          o._sign, o._coeff, o._exp) == 0

    def __ne__(self, o: Decimal) -> bool:
        return _cmp_parts(self._sign, self._coeff, self._exp,
                          o._sign, o._coeff, o._exp) != 0

    def __lt__(self, o: Decimal) -> bool:
        return _cmp_parts(self._sign, self._coeff, self._exp,
                          o._sign, o._coeff, o._exp) < 0

    def __le__(self, o: Decimal) -> bool:
        return _cmp_parts(self._sign, self._coeff, self._exp,
                          o._sign, o._coeff, o._exp) <= 0

    def __gt__(self, o: Decimal) -> bool:
        return _cmp_parts(self._sign, self._coeff, self._exp,
                          o._sign, o._coeff, o._exp) > 0

    def __ge__(self, o: Decimal) -> bool:
        return _cmp_parts(self._sign, self._coeff, self._exp,
                          o._sign, o._coeff, o._exp) >= 0

    def compare(self, o: Decimal) -> int:
        return _cmp_parts(self._sign, self._coeff, self._exp,
                          o._sign, o._coeff, o._exp)

    def __hash__(self) -> int:
        c: big = self._coeff
        e: int = self._exp
        sg: int = self._sign
        while c != 0 and c % 10 == 0:
            c = c // 10
            e = e + 1
        if c == 0:
            sg = 0
            e = 0
        return cast_i64(c * 31 + e + sg * 1000003)

    # ── 文本形式 ──────────────────────────────────────────────

    def __str__(self) -> str:
        ds: str = str(self._coeff)
        exp: int = self._exp
        nd: int = len(ds)
        adj: int = exp + nd - 1
        out: str = ""
        if exp <= 0 and adj >= -6:
            if exp == 0:
                out = ds
            else:
                point: int = nd + exp
                if point > 0:
                    out = ds[0:point] + "." + ds[point:nd]
                else:
                    zeros: str = ""
                    k: int = 0 - point
                    while k > 0:
                        zeros = zeros + "0"
                        k = k - 1
                    out = "0." + zeros + ds
        else:
            if nd == 1:
                out = ds + "E" + _exp_text(adj)
            else:
                out = ds[0:1] + "." + ds[1:nd] + "E" + _exp_text(adj)
        if self._sign == 1:
            out = "-" + out
        return out

    def __repr__(self) -> str:
        return "Decimal('" + str(self) + "')"

    # ── CPython 同名查询方法 ──────────────────────────────────

    def is_zero(self) -> bool:
        return self._coeff == 0

    def is_signed(self) -> bool:
        return self._sign == 1

    def copy_abs(self) -> Decimal:
        if self._coeff == 0:
            return _from_parts(0, 0, self._exp)
        return _from_parts(0, self._coeff, self._exp)

    def copy_negate(self) -> Decimal:
        return _from_parts(1 - self._sign, self._coeff, self._exp)

    def adjusted(self) -> int:
        if self._coeff == 0:
            return self._exp
        return self._exp + _ndigits(self._coeff) - 1

    def as_tuple(self) -> tuple[int, tuple[int, ...], int]:
        lst: list[int] = _digits_list(self._coeff)
        ds: tuple[int, ...] = tuple(lst)
        return (self._sign, ds, self._exp)

    def quantize(self, exp: Decimal, rounding: str = "") -> Decimal:
        mode: str = rounding
        if mode == "":
            mode = _context.rounding
        target: int = exp._exp
        sign: int = self._sign
        coeff: big = self._coeff
        cur: int = self._exp
        if cur >= target:
            coeff = coeff * _pow10(cur - target)
        else:
            coeff = _round_mag(coeff, target - cur, mode, False)
        if coeff == 0:
            sign = self._sign
        if _ndigits(coeff) > _context.prec:
            raise ValueError("quantize result has too many digits")
        return _from_parts(sign, coeff, target)
