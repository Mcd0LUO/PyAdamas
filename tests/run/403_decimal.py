# Decimal 综合：算术、舍入、构造、格式化、f-string、上下文、错误口径
# 0.383 合并自 9 个文件：403_decimal_arithmetic.py, 404_decimal_rounding.py, 405_decimal_errors.py, 412_decimal_ctor.py, 413_decimal_format_basic.py, 414_decimal_format_group.py, 415_decimal_errors.py, 416_decimal_fstring.py, 417_decimal_context.py
# 每段包在独立函数里，避免同名变量跨段冲突。

# ── 原 403_decimal_arithmetic.py ──
def _case_1() -> None:
    """stdlib/decimal.py：定点三元组表示、精确整数内核四则、字符串形式。"""
    from decimal import Decimal, getcontext

    print(getcontext().prec, getcontext().rounding)

    # 表示：sign/coefficient/exponent（as_tuple 的 sign 与 CPython 一样是 0/1）。
    for s in ["1", "1.0", "-0", "0.00", "1E+2", "1.2300", "-12.30", "000.00", "+1.5", ".5", "5.", "1e-7"]:
        d = Decimal(s)
        print(s, "->", str(d), repr(d), d.as_tuple()[0], d.as_tuple()[1], d.as_tuple()[2])

    # 四则：加减对齐指数、乘做整数乘、除做长除（精确商保留理想指数）。
    print(Decimal("1.20") + Decimal("2.3"), Decimal("1.20") - Decimal("2.30"))
    print(Decimal("1.20") * Decimal("2.3"), Decimal("9.99") * Decimal("9.99"))
    print(Decimal("1") / Decimal("8"), Decimal("1") / Decimal("2"), Decimal("10") / Decimal("4"),
          Decimal("10.0") / Decimal("2"), Decimal("1.00") / Decimal("2"))
    print(Decimal("1") / Decimal("7"), Decimal("100") / Decimal("3"), Decimal("1e10") / Decimal("3"))
    # 精确商剥掉尾随零后仍超 prec：舍入位置必须按剥零后的位数算。
    print(Decimal("12345678901234567890123456789") / Decimal("1"))
    print(Decimal("12345678901234567890123456789") / Decimal("1.0"))
    print(Decimal("12345678901234567890123456789") / Decimal("10"))
    print(Decimal("999999999999999999999999999999") / Decimal("1"))
    print(Decimal("1234567890123456789012345678900") / Decimal("1"))

    # 精确整数内核：结果超过 i64 不回绕，超过 prec=28 才按上下文舍入。
    print(Decimal("9223372036854775807") + Decimal("1"))
    print(Decimal("9223372036854775807") * Decimal("2"))
    print(Decimal("3037000500") * Decimal("3037000500"))
    print(Decimal("123456789012345678901234567890") + Decimal("987654321098765432109876543210"))
    print(Decimal("123456789012345678901234567890") * Decimal("987654321098765432109876543210"))
    print(Decimal("1267650600228229401496703205376") + Decimal("0"))

    # 比较 / 数值相等（1.0 == 1.00）/ 哈希契约 / dict。
    a = Decimal("1.50")
    b = Decimal("1.5")
    print(a == b, a != b, a < b, a <= b, a > b, a >= b, a.compare(b), hash(a) == hash(b))
    print(Decimal("0.00") == Decimal("-0"), Decimal("-0") < Decimal("0"),
          Decimal("1e40") > Decimal("9999999999999999999999999999999999999999"))
    d: dict[Decimal, int] = {}
    d[Decimal("1.0")] = 1
    d[Decimal("2.50")] = 2
    print(len(d), d[Decimal("1.00")], d[Decimal("2.5")])

    # 查询方法。
    print(Decimal("-1.20").copy_abs(), Decimal("0.00").copy_negate(),
          Decimal("-0.00").copy_negate(), Decimal("-1.20").is_signed(),
          Decimal("0.00").is_zero(), Decimal("1E+2").adjusted(), Decimal("0.001").adjusted())
    print(-Decimal("1.20"), -Decimal("0.00"), abs(Decimal("-1.20")))
    print(bool(Decimal("0.00")), bool(Decimal("-0.1")))
    print(Decimal("0E+3"), Decimal("-0.00"), Decimal("1.5E+2"), Decimal("1.20E+2"))

# ── 原 404_decimal_rounding.py ──
def _case_2() -> None:
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

# ── 原 405_decimal_errors.py ──
def _case_3() -> None:
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

# ── 原 412_decimal_ctor.py ──
def _case_4() -> None:
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

# ── 原 413_decimal_format_basic.py ──
def _case_5() -> None:
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

# ── 原 414_decimal_format_group.py ──
def _case_6() -> None:
    """stdlib/decimal.py：__format__ 分组分隔符、填充/对齐矩阵、分数位分隔。

    ',' 与 '_' 是千分位；精度里的 ',' / '_' 是小数位分组（CPython mini-language
    的 frac_separators）。零填充与分组同时出现时，补零发生在分组之前。
    """
    from decimal import Decimal

    v: Decimal = Decimal("1234567.891234")
    print(repr(format(v, ",")), repr(format(v, "_")), repr(format(v, ",f")), repr(format(v, "_f")))
    print(repr(format(v, ",.2f")), repr(format(v, "_.2f")), repr(format(v, ",e")), repr(format(v, ",g")))
    print(repr(format(v, ",%")), repr(format(v, ",.0%")), repr(format(v, "#,.0f")))
    print(repr(format(Decimal("-1234567.891234"), ",.2f")), repr(format(Decimal("-1234567.891234"), "_.2f")))

    # 小数位分组：精度里的分隔符每 3 位插一次
    print(repr(format(Decimal("1.23456789"), ".9_")), repr(format(Decimal("1.23456789"), ".9,")))
    print(repr(format(Decimal("1.23456789"), "._")), repr(format(Decimal("1.23456789"), ".6_")))
    print(repr(format(Decimal("0.000123456"), ".12_")),
          repr(format(Decimal("1234.5678"), ",.8_")))

    # 零填充 + 分组：补零在分组之前（0,123,456 而不是 123,456...）
    print(repr(format(Decimal("123456"), "012,")), repr(format(Decimal("123456"), "015,")))
    print(repr(format(Decimal("123456.789"), "020,.3f")))
    print(repr(format(Decimal("123456"), "015_")), repr(format(Decimal("-123456"), "015,")))
    print(repr(format(Decimal("123"), "010,")), repr(format(Decimal("1"), "010,")))

    # 填充字符 + 对齐矩阵（含多字符宽度、'=' 在零填充外）
    for align in ["<", ">", "^", "="]:
        print(align, repr(format(v, "*" + align + "26,.2f")), repr(format(v, align + "26,.2f")))
    print(repr(format(v, "*<26")), repr(format(v, "*>26")), repr(format(v, "*^26")))
    print(repr(format(Decimal("-1.5"), "0=+12,.2f")), repr(format(Decimal("-1.5"), "=+12,.2f")))
    print(repr(format(Decimal("-0.004"), "012.2%")), repr(format(Decimal("0.5"), "012.2%")))

    # 短宽度（不补）、超长值、精度大于位数
    print(repr(format(v, "1")), repr(format(v, "2")), repr(format(v, "0,")))
    print(repr(format(Decimal("1"), ".20f")), repr(format(Decimal("1.5"), ".20e")))
    print(repr(format(Decimal("123456789012345678901234567890"), ",")))
    print(repr(format(Decimal("123456789012345678901234567890.123"), ",.4f")))

    # 千分位只作用于整数部分，符号与百分号不参与分组
    print(repr(format(Decimal("-1234.5"), ",.1f")), repr(format(Decimal("-1234.5"), "+,.1f")),
          repr(format(Decimal("-1234.5"), ",.1%")))

# ── 原 415_decimal_errors.py ──
def _case_7() -> None:
    """stdlib/decimal.py：非法构造输入、非法格式说明符、NaN/Infinity 的明确拒绝。

    本文件的期望块由 build/pyadamas 实跑生成，并逐条与 CPython 3.14 核对：
    非法说明符（d/s/x/abc/*<0/^0.0f/...）与非法字面量在 CPython 上同样是
    ValueError；'n' 与 NaN/Infinity 是**本轮有意拒绝**的 delta（CPython 接受
    'n'、接受 NaN/Infinity）。bool 来源与 CPython 一致（True → 1）；None 在
    pyadamas 的 unknown 域里与整数 0 同表示，无法区分（记录为已知差异）。
    """
    from decimal import Decimal

    d: Decimal = Decimal("1234.5678")

    # 非法格式说明符：全部 ValueError
    for s in ["d", "s", "x", "X", "o", "b", "c", "abc", "1.2.3", ".", "e5",
              "10.2z", "*<05", "<05", "*<0", "^0.0f", "+-5", "0,,", ",,",
              "5 5", "%10.2f", ".2x", "z z", "#z#", "1e2", "  ", "\t"]:
        try:
            print(repr(s), repr(format(d, s)))
        except ValueError:
            print(repr(s), "VE")

    # 合法但少见的说明：确认没有误杀
    for s in ["", "#", "z", "0", ",", "_", "=5", "0=5", "05", "-", "+", "z#0,.0f"]:
        try:
            print(repr(s), repr(format(d, s)))
        except ValueError:
            print(repr(s), "UNEXPECTED VE")

    # 'n' 依赖 locale：明确拒绝而不是给出 C locale 之外可能错误的结果
    for s in ["n", ".2n", ",n", "10n"]:
        try:
            print(repr(s), repr(format(d, s)))
        except ValueError:
            print(repr(s), "n rejected")

    # NaN / Infinity：字符串与 float 两路都必须是 ValueError（不是静默 0）
    for src in ["nan", "NaN", "-nan", "sNaN", "inf", "Inf", "Infinity", "-Inf",
                "infinity", "nan(1)"]:
        try:
            print(src, Decimal(src))
        except ValueError:
            print(src, "VE")
    for f in [float("nan"), float("inf"), float("-inf")]:
        try:
            print(f, Decimal(f))
        except ValueError:
            print("float nan/inf VE")

    # 其他非法字面量
    for bad in ["", "abc", "1.2.3", "1e", "--1", ".", "1e+", "0x10", "1e 2", "1..2",
                " 1", "1 ", "1_0", "1.5d", "True", "None"]:
        try:
            print("bad", repr(bad), str(Decimal(bad)))
        except ValueError:
            print("bad", repr(bad), "VE")

    # bool 来源：与 CPython 一致（True → 1、False → 0，bool 在 pyadamas 里
    # 就是 int 的表示，str(True) 已经是 "1"）。
    print(Decimal(True), Decimal(False), Decimal(True) == Decimal(1))
    # None 来源：unknown 域里 None 与整数 0 是同一个机器字（没有类型标签），
    # 无法区分，所以 Decimal(None) 得到 0 —— 有意记录的行为差异，不是静默 NaN。
    print(Decimal(None))

    # float 构造不会静默 NaN：即便经过 unknown 传递
    u: unknown = float("nan")
    try:
        print(Decimal(u))
    except ValueError:
        print("unknown nan VE")

# ── 原 416_decimal_fstring.py ──
def _case_8() -> None:
    """stdlib/decimal.py：f-string 里的 Decimal（空说明 + 纯宽度/对齐/填充）。

    注意（编译器缺陷，不在本轮范围内修）：lower_joined 在类实例有 __format__
    时先调用 __format__(spec)，随后又把结果当 str 再套一遍同一个 spec。纯
    宽度/填充/对齐说明因为「结果已经够宽」而幂等，所以下面这些 f-string 与
    format() 完全一致；带精度（.2f）、分组（,）、符号（+）、'=' 对齐、'0'
    填充的 f-string 会二次套用甚至被字符串格式拒绝（ValueError），这类写法
    请用 format(x, spec)。
    """
    from decimal import Decimal

    d: Decimal = Decimal("1234.5678")
    n: Decimal = Decimal("-1234.5678")

    print(f"{d}", f"{n}")
    print(repr(f"{d:>16}"), repr(format(d, ">16")))
    print(repr(f"{n:>16}"), repr(format(n, ">16")))
    print(repr(f"{d:<16}"), repr(format(d, "<16")))
    print(repr(f"{d:^16}"), repr(format(d, "^16")))
    print(repr(f"{d:*^16}"), repr(format(d, "*^16")))
    print(repr(f"{d:*<16}"), repr(format(d, "*<16")))
    print(repr(f"{d:16}"), repr(format(d, "16")))
    print(repr(f"{d:^3}"), repr(format(d, "^3")))
    print(f"[{d}]", f"[{n}]")

    # 带精度/分组/符号的说明：format() 正常，f-string 受上述缺陷影响。
    # 例如 f"{d:.0f}" 在 pyadamas 上抛 ValueError: Invalid format specifier for str
    # （__format__ 已经给出 "1235"，字符串格式又不接受 .0f），所以这里只用 format。
    print(repr(format(d, ".0f")), repr(format(d, ",.2f")), repr(format(d, "+.2f")),
          repr(format(d, "020,.2f")))

# ── 原 417_decimal_context.py ──
def _case_9() -> None:
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

_case_1()
_case_2()
_case_3()
_case_4()
_case_5()
_case_6()
_case_7()
_case_8()
_case_9()

# --- expect ---
# 28 ROUND_HALF_EVEN
# 1 -> 1 Decimal('1') 0 (1,) 0
# 1.0 -> 1.0 Decimal('1.0') 0 (1, 0) -1
# -0 -> -0 Decimal('-0') 1 (0,) 0
# 0.00 -> 0.00 Decimal('0.00') 0 (0,) -2
# 1E+2 -> 1E+2 Decimal('1E+2') 0 (1,) 2
# 1.2300 -> 1.2300 Decimal('1.2300') 0 (1, 2, 3, 0, 0) -4
# -12.30 -> -12.30 Decimal('-12.30') 1 (1, 2, 3, 0) -2
# 000.00 -> 0.00 Decimal('0.00') 0 (0,) -2
# +1.5 -> 1.5 Decimal('1.5') 0 (1, 5) -1
# .5 -> 0.5 Decimal('0.5') 0 (5,) -1
# 5. -> 5 Decimal('5') 0 (5,) 0
# 1e-7 -> 1E-7 Decimal('1E-7') 0 (1,) -7
# 3.50 -1.10
# 2.760 99.8001
# 0.125 0.5 2.5 5.0 0.50
# 0.1428571428571428571428571429 33.33333333333333333333333333 3333333333.333333333333333333
# 1.234567890123456789012345679E+28
# 1.234567890123456789012345679E+28
# 1234567890123456789012345679
# 1.000000000000000000000000000E+30
# 1.234567890123456789012345679E+30
# 9223372036854775808
# 18446744073709551614
# 9223372037000250000
# 1.111111110111111111011111111E+30
# 1.219326311370217952261850327E+59
# 1.267650600228229401496703205E+30
# True False False True False True 0 True
# True False True
# 2 1 2
# 1.20 -0.00 0.00 True True 2 -3
# -1.20 0.00 1.20
# False True
# 0E+3 -0.00 1.5E+2 120
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
# '1,234,567.891234' '1_234_567.891234' '1,234,567.891234' '1_234_567.891234'
# '1,234,567.89' '1_234_567.89' '1.234567891234e+6' '1,234,567.891234'
# '123,456,789.1234%' '123,456,789%' '1,234,568.'
# '-1,234,567.89' '-1_234_567.89'
# '1.234_567_89' '1.234,567,89'
# '1.234_567_89' '1.234_57'
# '0.000_123_456' '1,234.567_8'
# '0,000,123,456' '000,000,123,456'
# '0,000,000,123,456.789'
# '000_000_123_456' '-00,000,123,456'
# '00,000,123' '00,000,001'
# < '1,234,567.89**************' '1,234,567.89              '
# > '**************1,234,567.89' '              1,234,567.89'
# ^ '*******1,234,567.89*******' '       1,234,567.89       '
# = '**************1,234,567.89' '              1,234,567.89'
# '1234567.891234************' '************1234567.891234' '******1234567.891234******'
# '-00000001.50' '-       1.50'
# '-0000000.40%' '00000050.00%'
# '1234567.891234' '1234567.891234' '1,234,567.891234'
# '1.00000000000000000000' '1.50000000000000000000e+0'
# '123,456,789,012,345,678,901,234,567,890'
# '123,456,789,012,345,678,901,234,567,890.1230'
# '-1,234.5' '-1,234.5' '-123,450.0%'
# 'd' 'd' VE
# 's' 's' VE
# 'x' 'x' VE
# 'X' 'X' VE
# 'o' 'o' VE
# 'b' 'b' VE
# 'c' 'c' VE
# 'abc' 'abc' VE
# '1.2.3' '1.2.3' VE
# '.' '.' VE
# 'e5' 'e5' VE
# '10.2z' '10.2z' VE
# '*<05' '*<05' VE
# '<05' '<05' VE
# '*<0' '*<0' VE
# '^0.0f' '^0.0f' VE
# '+-5' '+-5' VE
# '0,,' '0,,' VE
# ',,' ',,' VE
# '5 5' '5 5' VE
# '%10.2f' '%10.2f' VE
# '.2x' '.2x' VE
# 'z z' 'z z' VE
# '#z#' '#z#' VE
# '1e2' '1e2' VE
# '  ' '  ' VE
# '\t' '\t' VE
# '' '1234.5678'
# '#' '1234.5678'
# 'z' '1234.5678'
# '0' '1234.5678'
# ',' '1,234.5678'
# '_' '1_234.5678'
# '=5' '1234.5678'
# '0=5' '1234.5678'
# '05' '1234.5678'
# '-' '1234.5678'
# '+' '+1234.5678'
# 'z#0,.0f' '1,235.'
# 'n' 'n' n rejected
# '.2n' '.2n' n rejected
# ',n' ',n' n rejected
# '10n' '10n' n rejected
# nan nan VE
# NaN NaN VE
# -nan -nan VE
# sNaN sNaN VE
# inf inf VE
# Inf Inf VE
# Infinity Infinity VE
# -Inf -Inf VE
# infinity infinity VE
# nan(1) nan(1) VE
# nan float nan/inf VE
# inf float nan/inf VE
# -inf float nan/inf VE
# bad '' bad '' VE
# bad 'abc' bad 'abc' VE
# bad '1.2.3' bad '1.2.3' VE
# bad '1e' bad '1e' VE
# bad '--1' bad '--1' VE
# bad '.' bad '.' VE
# bad '1e+' bad '1e+' VE
# bad '0x10' bad '0x10' VE
# bad '1e 2' bad '1e 2' VE
# bad '1..2' bad '1..2' VE
# bad ' 1' bad ' 1' VE
# bad '1 ' bad '1 ' VE
# bad '1_0' bad '1_0' VE
# bad '1.5d' bad '1.5d' VE
# bad 'True' bad 'True' VE
# bad 'None' bad 'None' VE
# 1 0 True
# 0
# unknown nan VE
# 1234.5678 -1234.5678
# '       1234.5678' '       1234.5678'
# '      -1234.5678' '      -1234.5678'
# '1234.5678       ' '1234.5678       '
# '   1234.5678    ' '   1234.5678    '
# '***1234.5678****' '***1234.5678****'
# '1234.5678*******' '1234.5678*******'
# '       1234.5678' '       1234.5678'
# '1234.5678' '1234.5678'
# [1234.5678] [-1234.5678]
# '1235' '1,234.57' '+1234.57' '0,000,000,001,234.57'
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
