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

## 构造口径

五种来源统一先取 `str(value)` 得到 ASCII 十进制文本，再走同一条解析路径：

| 来源 | 文本来源 | 结果 |
|---|---|---|
| str | 原文（严格 ASCII 十进制） | 原样解析 |
| int / big | 十进制展开 | 精确 |
| float | repr 的最短十进制表示 | Decimal(repr(x))（**不是**二进制展开） |
| Decimal | `__str__`（无损往返） | 完全相同 |

`Decimal(value, context)` 的第二参数只做兼容：接受任意值并**忽略**
（pyadamas 没有上下文管理器/线程本地上下文，构造也不做上下文舍入）。
传第三个实参是编译期错误。bool 与 CPython 一致（`str(True)` 就是 "1"，
所以 Decimal(True) == 1、Decimal(False) == 0）；**None 是已知差异**：
unknown 域里 None 与整数 0 是同一个机器字（没有类型标签），无法区分，
Decimal(None) 得到 0（CPython 抛 TypeError）。

## 格式化口径

`__format__` 对齐 CPython 3.14 的 `decimal.__format__`（`_pydecimal` 与
C `_decimal` 实测逐字节一致）：解析 → 按 precision 舍入 → 定位小数点 →
分组/补符号/填充。

- 支持：空说明（等价 'G'，CPython 3.14 的 DefaultContext.capitals == 1）、
  类型 `e E f F g G %`、精度 `.N`（可带 `,`/`_` 分数位分组）、
  符号 `+`/`-`/空格、宽度、填充与对齐 `</>/^/=`、零填充 `0`、
  千分位 `,` 与 `_`、备用形式 `#`（强制小数点）、`z`（负零不带号）。
- 精度与舍入：'e/E' 按 `precision+1` 位**有效数字**舍入；'f/F/%' 按
  `-precision` 定点**重标定**；'g/G' 只在位数超过 precision 时按有效位
  舍入。舍入模式永远取 `getcontext().rounding`（CPython 同样忽略说明里的
  隐式模式）。这些路径不检查 `prec`（与 `quantize` 不同）。
- 明确拒绝（`ValueError("invalid format string")`，与 C 实现同一句话）：
  任何非法说明符（多余字符、`0` 与 fill/align 冲突、':' 之外的非 mini-
  language 字符）、未知类型（`d s x o b c` 等）、`.` 后没有数字/分隔符。
- 'n' 单独拒绝：它依赖 locale（thousands_sep/decimal_point），pyadamas 没有
  locale 模块，宁可报错也不输出 C locale 之外错误的结果。

## 明确不做的 CPython 行为（不假装完整兼容）

- 没有上下文管理器 localcontext()、没有线程本地上下文、没有 setcontext()。
- 没有 traps / signals / flags / Emin / Emax / Subnormal / Underflow：
  不做指数范围检查，不设置 Inexact/Rounded 之类的上下文标志。
- 没有 NaN、sNaN、Infinity、-Infinity；字符串或数值里出现这些直接
  ValueError（消息里点明「不支持 NaN/Infinity」），不会静默成 0。
- 构造接受 str / int / big / float / Decimal 五种来源，以及
  `Decimal(value, context)` 两参形式（context 只做兼容，接受后忽略，
  详见下面「构造口径」）。
- **float 构造走「最短十进制表示再解析」**：等价于 Decimal(repr(x))，
  不是 CPython 3.14 的二进制精确展开。Decimal(1.1) 因此是 1.1
  （CPython 是 1.100000000000000088817841970012523233890533447265625）。
  Decimal(3.0) 是 3.0（保留 repr 的 .0），不是 3。
- 构造不做上下文舍入：CPython 的 Decimal(float) 会按 context.prec 舍入，
  这里一律精确取 repr 文本。
- 不支持与 int/float/big 的混合运算或比较（先显式转成 Decimal 字符串），
  也不支持 `**`、`//`、`%`、`+`/`-` 一元正号、round()、math 函数。
- `as_tuple()` 返回普通 tuple 而不是 DecimalTuple 具名元组，
  `compare()` 返回 int -1/0/1 而不是 Decimal。
- `__format__` 已实现（见下面「格式化口径」），但 **f-string 带格式说明时
  编译器会对类实例二次套用格式**（`lower_joined` 在调用 `__format__` 之后
  又按 str 走了一遍 `FmtStr`）。纯宽度/对齐说明因为结果已够宽而恰好幂等，
  带精度/分组/符号的说明会二次套用甚至报错；这是编译器缺陷，不在本模块
  范围内修。`format(x, spec)` 与 `f"{x}"`（空说明）都正确。
- 字符串解析是严格 ASCII 十进制：不接受首尾空白、下划线和 Unicode 数字
  （CPython 容忍前两者）；指数不设上下限（CPython 有 Emax/Emin），超大
  指数差会真的分配大整数。`prec` 字段不做合法性校验（CPython 会拒绝
  < 1 的值）；未知舍入模式抛 ValueError（CPython 同样拒绝）。
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

    def copy(self) -> Context:
        """复制一份上下文（CPython 的 Context.copy 口径）。"""
        return Context(self.prec, self.rounding)

    def create_decimal(self, value: unknown) -> Decimal:
        """按**本**上下文构造 Decimal：构造后按 prec 舍入（CPython 口径）。"""
        d: Decimal = Decimal(value)
        d._apply_context(self)
        return d


_context: Context = Context()


def getcontext() -> Context:
    """返回当前上下文（默认 prec=28, ROUND_HALF_EVEN）。"""
    return _context


def setcontext(ctx: Context) -> None:
    """替换当前上下文（CPython 同口径：之后所有运算用新的 prec/rounding）。"""
    global _context
    _context = ctx


class _LocalContext:
    """localcontext() 的上下文管理器：进入时换上下文，退出时还原。

    异常路径也要还原（__exit__ 无条件执行），否则一次异常会把模块级上下文
    永久改掉。返回值 False 表示异常继续向上抛。
    """

    _saved: Context
    _current: Context

    def __init__(self, current: Context) -> None:
        self._saved = Context()
        self._current = current

    def __enter__(self) -> Context:
        global _context
        self._saved = _context
        _context = self._current
        return _context

    def __exit__(self, exc_type: unknown, exc: unknown, tb: unknown) -> bool:
        global _context
        _context = self._saved
        return False


def localcontext(ctx: unknown = 0) -> _LocalContext:
    """临时上下文：`with localcontext() as c:` 用当前上下文的副本，
    或在里面改 `c.prec`；`with localcontext(Context(5)):` 用给定上下文。
    """
    # 形参静态是 unknown，unknown 上既不能调用户方法也不能读属性（codegen
    # 限制），所以显式传进来的 Context 直接沿用（CPython 会复制一份，这里有
    # 细微差异：在 with 里改字段会改到调用方的对象）；无参时才复制当前上下文
    # —— 那份必须复制，否则 with 里的修改会漏回外层。
    if isinstance(ctx, Context):
        return _LocalContext(ctx)
    return _LocalContext(_context.copy())


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
    if (mode != ROUND_HALF_EVEN and mode != ROUND_HALF_UP
            and mode != ROUND_DOWN and mode != ROUND_UP):
        raise ValueError("invalid rounding mode: " + mode)
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
    exp: int = ideal - k
    if r == 0:
        # 精确商：按 CPython 的理想指数保留尾随零（不把指数抬到 ideal 之上）。
        while exp < ideal and q % 10 == 0:
            q = q // 10
            exp = exp + 1
        if _ndigits(q) <= prec:
            return sign, q, exp
    # 注意：精确分支可能已经除掉若干尾随零，位数与指数都要用**剥零之后**
    # 的值重新算，否则会在错误的位置舍入（实测 29 位整数除以 1 掉一个数量级）。
    nq: int = _ndigits(q)
    drop: int = nq - prec
    if drop < 0:
        drop = 0
    q2: big = _round_mag(q, drop, mode, r != 0)
    exp2: int = exp + drop
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


# ── 格式化（__format__）的内部工具 ────────────────────────────────
# 全部作用在 (sign, coefficient, exponent) 三元组上，不构造 Decimal。
# 口径逐条对齐 CPython 3.14 的 _pydecimal（_decimal 与它实测同输出）。


def _is_digit(ch: str) -> bool:
    return ch >= "0" and ch <= "9"


def _is_align(ch: str) -> bool:
    return ch == "<" or ch == ">" or ch == "=" or ch == "^"


def _zeros(k: int) -> str:
    out: str = ""
    i: int = 0
    while i < k:
        out = out + "0"
        i = i + 1
    return out


def _fmt_rescale(sign: int, coeff: big, exp: int, target: int,
                 mode: str) -> tuple[int, big, int]:
    """把指数重标定到 target（CPython Decimal._rescale 口径，不检查 prec）。"""
    s: int = sign
    c: big = coeff
    e: int = target
    if c == 0:
        return s, c, e
    if exp >= target:
        c = c * _pow10(exp - target)
        return s, c, e
    keep: int = _ndigits(c) + exp - target
    if keep < 0:
        # 值 < 10**(target-1)：半程/截断都归零，只有「远离零」的 ROUND_UP 进位。
        z: big = 0
        if mode == ROUND_UP:
            z = 1
        c = z
        return s, c, e
    c = _round_mag(c, _ndigits(c) - keep, mode, False)
    return s, c, e


def _fmt_round(sign: int, coeff: big, exp: int, places: int,
               mode: str) -> tuple[int, big, int]:
    """按有效数字位数 places 舍入（CPython Decimal._round 口径）。"""
    s: int = sign
    c: big = coeff
    e: int = exp
    if c == 0:
        return s, c, e
    adj: int = e + _ndigits(c) - 1
    s, c, e = _fmt_rescale(s, c, e, adj + 1 - places, mode)
    if c != 0:
        nadj: int = e + _ndigits(c) - 1
        if nadj != adj:
            # 进位改变了 adjusted（99.97 → 100.0），再收一次尾随零。
            s, c, e = _fmt_rescale(s, c, e, nadj + 1 - places, mode)
    return s, c, e


def _fmt_sign(neg: int, sign: str) -> str:
    if neg == 1:
        return "-"
    if sign == "+" or sign == " ":
        return sign
    return ""


def _fmt_group(digits: str, sep: str, min_width: int) -> str:
    """每 3 位插入 sep；min_width > 0 时左侧补零（_insert_thousands_sep 口径）。"""
    out: str = ""
    d: str = digits
    mw: int = min_width
    while True:
        ln: int = len(d)
        w: int = mw
        if ln > w:
            w = ln
        if w < 1:
            w = 1
        if w > 3:
            w = 3
        part: str = _zeros(w - ln)
        if ln > w:
            part = part + d[ln - w:ln]
            d = d[0:ln - w]
        else:
            part = part + d
            d = ""
        if out == "":
            out = part
        else:
            out = part + sep + out
        mw = mw - w
        if d == "" and mw <= 0:
            break
        mw = mw - len(sep)
    return out


def _fmt_align(sign: str, body: str, fill: str, align: str,
               width: int) -> str:
    """补到 width 宽（_format_align 口径）。"""
    pad: int = width - len(sign) - len(body)
    if pad <= 0:
        return sign + body
    padding: str = fill * pad
    if align == "<":
        return sign + body + padding
    if align == "=":
        return sign + padding + body
    if align == "^":
        half: int = pad // 2
        return padding[0:half] + sign + body + padding[half:pad]
    return padding + sign + body


def _fmt_body(neg: int, intpart: str, fracpart: str, exp: int, ftype: str,
              sign: str, alt: bool, zeropad: bool, width: int, sep: str,
              fsep: str, fill: str, align: str) -> str:
    """拼装小数部分、指数、% 号，再分组与填充（_format_number 口径）。"""
    sgn: str = _fmt_sign(neg, sign)
    fp: str = fracpart
    if fp != "" and fsep != "":
        grouped: str = ""
        pos: int = 0
        while pos < len(fp):
            chunk: str = fp[pos:pos + 3]
            if grouped == "":
                grouped = chunk
            else:
                grouped = grouped + fsep + chunk
            pos = pos + 3
        fp = grouped
    if fp != "" or alt:
        fp = "." + fp
    if exp != 0 or ftype == "e" or ftype == "E":
        ech: str = "e"
        if ftype == "E" or ftype == "G":
            ech = "E"
        if exp < 0:
            fp = fp + ech + "-" + str(0 - exp)
        else:
            fp = fp + ech + "+" + str(exp)
    if ftype == "%":
        fp = fp + "%"
    mw: int = 0
    if zeropad:
        mw = width - len(fp) - len(sgn)
    return _fmt_align(sgn, _fmt_group(intpart, sep, mw) + fp, fill, align,
                      width)


class Decimal:
    """定点十进制数（见模块 docstring 的表示与边界）。"""

    _sign: int
    _coeff: big
    _exp: int

    def __init__(self, value: unknown = "0", context: unknown = 0) -> None:
        # 五种来源：int/big 走 str() 的精确十进制、float 走 str() 的最短
        # repr、str 原文、Decimal 直接拷三元组（三者在 unknown 域都能认）。
        # context 只做兼容，接受后忽略。
        self._sign = 0
        self._coeff = 0
        self._exp = 0
        if isinstance(value, Decimal):
            self._copy_from(value)
        else:
            self._parse(str(value))
        # context 参数：CPython 3.14 实测对 str/int/big/Decimal 都不做精度
        # 舍入（连 Decimal(float, ctx) 也不按 prec 舍），所以这里只接受、
        # 不舍入；真正按上下文舍入的入口是 Context.create_decimal。
        # 保留这个形参是为了签名兼容（Decimal(v, ctx) 到处都在用）。

    def _apply_context(self, ctx: Context) -> None:
        """按给定上下文舍入到 prec 位有效数字。"""
        self._sign, self._coeff, self._exp = _round_parts(
            self._sign, self._coeff, self._exp, ctx.prec, ctx.rounding)


    def _copy_from(self, o: Decimal) -> None:
        """复制构造：三元组原样拷贝（不经过字符串，指数与尾随零都保住）。"""
        self._sign = o._sign
        self._coeff = o._coeff
        self._exp = o._exp

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
            # NaN/Infinity 本轮不支持，但要说清楚是「不支持」而不是「非法」。
            if int_start < n:
                c0: str = s[int_start]
                if c0 == "n" or c0 == "N" or c0 == "i" or c0 == "I":
                    raise ValueError(
                        "Decimal(): NaN and Infinity are not supported")
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

    # ── PEP 3101 格式化 ───────────────────────────────────────

    def __format__(self, spec: str) -> str:
        """按 CPython 3.14 Decimal.__format__ 的说明符排版（见模块头）。"""
        fill: str = " "
        align: str = ">"
        sign: str = "-"
        ftype: str = ""
        sep: str = ""
        fsep: str = ""
        width: int = 0
        precision: int = -1
        zeropad: bool = False
        alt: bool = False
        no_neg_0: bool = False
        has_fill: bool = False
        has_align: bool = False
        n: int = len(spec)
        i: int = 0
        # [[fill]align]
        if n >= 2 and _is_align(spec[1]):
            fill = spec[0]
            align = spec[1]
            has_fill = True
            has_align = True
            i = 2
        elif n >= 1 and _is_align(spec[0]):
            align = spec[0]
            has_align = True
            i = 1
        # sign
        if i < n:
            ch: str = spec[i]
            if ch == "+" or ch == " " or ch == "-":
                sign = ch
                i = i + 1
        # 'z'：负零不显示符号（3.11+）
        if i < n and spec[i] == "z":
            no_neg_0 = True
            i = i + 1
        # '#'：强制小数点
        if i < n and spec[i] == "#":
            alt = True
            i = i + 1
        # '0'：零填充，不能与 fill/align 同时出现
        if i < n and spec[i] == "0":
            zeropad = True
            i = i + 1
        if zeropad and (has_fill or has_align):
            raise ValueError("invalid format string")
        # width
        ws: int = i
        while i < n and _is_digit(spec[i]):
            i = i + 1
        if i > ws:
            width = int(spec[ws:i])
        # 千分位分隔符
        if i < n and (spec[i] == "," or spec[i] == "_"):
            sep = spec[i]
            i = i + 1
        # 精度（'.' 后面必须紧跟数字或分隔符，否则整条说明非法）
        if i + 1 < n and spec[i] == ".":
            if _is_digit(spec[i + 1]) or spec[i + 1] == "," or spec[i + 1] == "_":
                i = i + 1
                ps: int = i
                while i < n and _is_digit(spec[i]):
                    i = i + 1
                if i > ps:
                    precision = int(spec[ps:i])
                if i < n and (spec[i] == "," or spec[i] == "_"):
                    fsep = spec[i]
                    i = i + 1
        # type
        if i < n:
            ch = spec[i]
            if (ch == "e" or ch == "E" or ch == "f" or ch == "F" or ch == "g"
                    or ch == "G" or ch == "n" or ch == "%"):
                ftype = ch
                i = i + 1
        if i != n:
            raise ValueError("invalid format string")
        # 精度 0 对 g/G/空说明没有意义，CPython 归一成 1。
        if precision == 0 and (ftype == "" or ftype == "g" or ftype == "G"
                               or ftype == "n"):
            precision = 1
        if ftype == "":
            # CPython 3.14 的 DefaultContext.capitals == 1 → 空说明按 'G'。
            ftype = "G"
        if ftype == "n":
            raise ValueError("decimal format type 'n' needs locale; "
                             "not supported")
        mode: str = _context.rounding
        s: int = self._sign
        c: big = self._coeff
        e: int = self._exp
        if ftype == "%":
            e = e + 2
        # 按 precision 舍入：e/E 按有效位，f/F/% 按定点，g/G 只在超位数时。
        if precision >= 0:
            if ftype == "e" or ftype == "E":
                s, c, e = _fmt_round(s, c, e, precision + 1, mode)
            elif ftype == "f" or ftype == "F" or ftype == "%":
                s, c, e = _fmt_rescale(s, c, e, 0 - precision, mode)
            else:
                if _ndigits(c) > precision:
                    s, c, e = _fmt_round(s, c, e, precision, mode)
        # 正指数的零没法写成定点：归到 0E+0。
        if c == 0 and e > 0 and (ftype == "f" or ftype == "F"
                                 or ftype == "%"):
            s, c, e = _fmt_rescale(s, c, e, 0, mode)
        asg: int = s
        if c == 0 and no_neg_0 and s == 1:
            asg = 0
        # 小数点位置
        nd: int = _ndigits(c)
        left: int = e + nd
        dot: int = 1
        if ftype == "e" or ftype == "E":
            if c == 0 and precision >= 0:
                dot = 1 - precision
        elif ftype == "f" or ftype == "F" or ftype == "%":
            dot = left
        else:
            if e <= 0 and left > -6:
                dot = left
        ds: str = str(c)
        intpart: str = ""
        fracpart: str = ""
        if dot < 0:
            intpart = "0"
            fracpart = _zeros(0 - dot) + ds
        elif dot > nd:
            intpart = ds + _zeros(dot - nd)
        else:
            intpart = ds[0:dot]
            if intpart == "":
                intpart = "0"
            fracpart = ds[dot:nd]
        return _fmt_body(asg, intpart, fracpart, left - dot, ftype, sign,
                         alt, zeropad, width, sep, fsep, fill, align)

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

    def to_integral_value(self, rounding: str = "") -> Decimal:
        """四舍五入到整数（CPython Decimal.to_integral_value 口径）。"""
        return self.quantize(_from_parts(0, 1, 0), rounding)

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
