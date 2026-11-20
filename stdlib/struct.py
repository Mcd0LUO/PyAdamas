"""struct —— 二进制打包/解包（纯 pyadamas 源码，stdlib3）。

覆盖 pack / unpack / unpack_from / calcsize / iter_unpack，以及 error。
支持格式前缀 < > ! (字节序) 与 = @，类型码 x ? b B h H i I l L q Q f d
c s p，计数前缀（如 4s / 2i）。

语义边界（轻量标尺，见 docs/stdlib-batch3.md）：
- 没有 pack_into：pa 的 bytes 不可变，没有可写缓冲原语（用 pack + 拼接）；
- 整数是 int64：Q/L 解出高位为 1 的数会按 int64 回绕成负数（CPython 是
  任意精度）；pack 侧接受负数按补码写入；
- '<' '>' '!' '=' 用标准尺寸（l=4）；只有 '@' 用本机尺寸（x86_64 上 l=8）
  并对成员按自身尺寸对齐（同 CPython native 口径）；
- 's' / 'p' 收 bytes 或 str：pa 的 bytes 与 str 共用运行期表示、unknown
  实参上分不出来，bytes 实际按「码点 → UTF-8」再打包（ASCII 完全一致，
  非 UTF-8 字节串请在 pack 前自己 encode；反解出的 's'/p 值按 bytes 标记，
  但 print 会按 str 渲染）；
- unpack / iter_unpack / unpack_from 的具体返回类型：unpack* 是元组，
  iter_unpack 返回 list（CPython 是迭代器，文档化的差异）。
"""


import math


class error(Exception):
    """struct 的错误（格式错、参数不匹配）。"""


_NATIVE_SIZES = {
    "?": 1, "b": 1, "B": 1, "h": 2, "H": 2, "i": 4, "I": 4,
    "l": 8, "L": 8, "q": 8, "Q": 8, "f": 4, "d": 8,
}
_STD_SIZES = {
    "?": 1, "b": 1, "B": 1, "h": 2, "H": 2, "i": 4, "I": 4,
    "l": 4, "L": 4, "q": 8, "Q": 8, "f": 4, "d": 8,
}
_INT_CODES = "bBhHiIlLqQ"
_SIGNED_CODES = "bhilq"


class _Layout:
    big: bool
    native: bool
    align: bool
    codes: list[str]
    counts: list[int]
    offsets: list[int]
    size: int

    def __init__(self) -> None:
        self.big = False
        self.native = False
        self.align = False
        self.codes = []
        self.counts = []
        self.offsets = []
        self.size = 0


def _item_size(code: str, native: bool) -> int:
    if code == "s" or code == "p" or code == "c" or code == "x" or code == "?":
        return 1
    if native:
        if code in _NATIVE_SIZES:
            return _NATIVE_SIZES[code]
    else:
        if code in _STD_SIZES:
            return _STD_SIZES[code]
    raise error("unknown format code " + repr(code))


def _parse(fmt: str) -> _Layout:
    lay = _Layout()
    i = 0
    while i < len(fmt):
        ch = fmt[i]
        if ch == " " or ch == "\t" or ch == "\n":
            i += 1
            continue
        if ch == "<":
            lay.big = False
            lay.native = False
            lay.align = False
            i += 1
            continue
        if ch == ">":
            lay.big = True
            lay.native = False
            lay.align = False
            i += 1
            continue
        if ch == "!":
            lay.big = True
            lay.native = False
            lay.align = False
            i += 1
            continue
        if ch == "=":
            # '=' 是「本机字节序 + 标准尺寸」，只有 '@' 用本机尺寸
            lay.native = False
            lay.align = False
            lay.big = False
            i += 1
            continue
        if ch == "@":
            lay.native = True
            lay.align = True
            lay.big = False
            i += 1
            continue
        n = 0
        digits = 0
        while i < len(fmt) and fmt[i].isdigit():
            n = n * 10 + int(fmt[i])
            i += 1
            digits += 1
        if digits == 0:
            n = 1
        if i >= len(fmt):
            raise error("bad format string")
        code = fmt[i]
        if code not in "xcbBhHiIlLqQfdcsp?":
            raise error("unknown format code " + repr(code))
        if code in "sp":
            if n > 1:
                pass
        offset = lay.size
        if lay.align and n == 1:
            sz = _item_size(code, lay.native)
            if sz > 1:
                pad = (sz - (offset % sz)) % sz
                offset = offset + pad
        if code in ("s", "p", "c"):
            step = 1
        else:
            step = _item_size(code, lay.native)
        if code == "s" or code == "p":
            offset = lay.size
            step = 1
        lay.codes.append(code)
        lay.counts.append(n)
        lay.offsets.append(offset)
        lay.size = offset + step * n
        i += 1
    return lay


def calcsize(fmt: str) -> int:
    """格式串对应的字节数。"""
    return _parse(fmt).size


def _to_int(x: unknown) -> int:
    return int(x)


def _to_float(x: unknown) -> float:
    return float(x)


def _str_to_bytes(s: str) -> bytes:
    return s.encode()


def _bytes_id(b: bytes) -> bytes:
    return b


def _as_bytes(x: unknown) -> bytes:
    if isinstance(x, str):
        return _str_to_bytes(x)
    return _bytes_id(x)


# ── IEEE 754 位级编解码（纯 Python，不依赖 C）────────────────────


def _d_bits(x: float) -> tuple[int, int]:
    """double -> (高位 32 位, 低位 32 位) 的位模式。"""
    sign = 0
    if math.copysign(1.0, x) < 0.0:
        sign = 1
    v = x
    if sign == 1:
        v = -v
    if v == 0.0:
        return (sign << 31, 0)
    if math.isinf(v):
        return ((sign << 31) | 0x7FF00000, 0)
    if math.isnan(v):
        return ((sign << 31) | 0x7FF80000, 1)
    m, e = math.frexp(v)
    if e >= -1021:
        expf = e + 1022
        mant = int(m * 9007199254740992.0) - 4503599627370496
        hi = (sign << 31) | (expf << 20) | (mant >> 32)
        lo = mant & 4294967295
        return (hi, lo)
    # 次正规数：mant = round(v / 2**-1074)
    mant = int(math.ldexp(m, e + 1074))
    return (sign << 31, mant & 4294967295)


def _bits_d(hi: int, lo: int) -> float:
    """位模式 -> double。"""
    sign = -1.0 if (hi & 2147483648) != 0 else 1.0
    expf = (hi >> 20) & 2047
    mant = (hi & 1048575) * 4294967296 + lo
    if expf == 2047:
        if mant == 0:
            return sign * math.inf
        return math.nan
    if expf == 0:
        return sign * math.ldexp(mant, -1074)
    return sign * math.ldexp(mant + 4503599627370496, expf - 1075)


def _f_bits(x: float) -> int:
    """float -> 32 位单精度位模式（就近舍入）。"""
    sign = 0
    if math.copysign(1.0, x) < 0.0:
        sign = 1
    v = x
    if sign == 1:
        v = -v
    if v == 0.0:
        return sign << 31
    if math.isinf(v):
        return (sign << 31) | 0x7F800000
    if math.isnan(v):
        return (sign << 31) | 0x7FC00000
    m, e = math.frexp(v)
    if e > 128:
        return (sign << 31) | 0x7F800000
    if e >= -125:
        y = m * 16777216.0
        mant = int(y)
        if y - float(mant) >= 0.5:
            mant = mant + 1
        if mant >= 16777216:
            mant = mant // 2
            e = e + 1
        expf = e + 126
        if expf >= 255:
            return (sign << 31) | 0x7F800000
        return (sign << 31) | (expf << 23) | (mant - 8388608)
    # 次正规
    y = math.ldexp(m, e + 149)
    mant = int(y)
    if y - float(mant) >= 0.5:
        mant = mant + 1
    if mant >= 8388608:
        return (sign << 31) | (1 << 23) | 0
    return (sign << 31) | mant


def _bits_f(bits: int) -> float:
    sign = -1.0 if (bits & 2147483648) != 0 else 1.0
    expf = (bits >> 23) & 255
    mant = bits & 8388607
    if expf == 255:
        if mant == 0:
            return sign * math.inf
        return math.nan
    if expf == 0:
        return sign * math.ldexp(mant, -149)
    return sign * math.ldexp(mant + 8388608, expf - 150)


# ── 字节组装 ────────────────────────────────────────────────────


def _bytes_of(v: int, n: int, big: bool) -> list[int]:
    out: list[int] = []
    i = 0
    if big:
        while i < n:
            shift = 8 * (n - 1 - i)
            out.append((v >> shift) & 255)
            i += 1
    else:
        while i < n:
            out.append((v >> (8 * i)) & 255)
            i += 1
    return out


def _read_uint(data: bytes, off: int, n: int, big: bool) -> int:
    v = 0
    i = 0
    if big:
        while i < n:
            v = (v << 8) | data[off + i]
            i += 1
    else:
        i = n - 1
        while i >= 0:
            v = (v << 8) | data[off + i]
            i -= 1
    return v


def _read_int(data: bytes, off: int, n: int, big: bool) -> int:
    v = _read_uint(data, off, n, big)
    if n == 8:
        return v
    if n == 1 and (v & 128) != 0:
        return v - 256
    if n == 2 and (v & 32768) != 0:
        return v - 65536
    if n == 4 and (v & 2147483648) != 0:
        return v - 4294967296
    return v


def _pad_to(buf: list[int], off: int) -> None:
    while len(buf) < off:
        buf.append(0)


# ── 打包 ────────────────────────────────────────────────────────


def pack(fmt: str, *args) -> bytes:
    """按格式串打包成 bytes。"""
    lay = _parse(fmt)
    buf: list[int] = []
    argi = 0
    total = len(args)
    for k in range(len(lay.codes)):
        code = lay.codes[k]
        count = lay.counts[k]
        off = lay.offsets[k]
        _pad_to(buf, off)
        if code in "bBhHiIlLqQ":
            if argi + count > total:
                raise error("pack expected more items for format")
            j = 0
            while j < count:
                v = _to_int(args[argi])
                argi += 1
                size = _item_size(code, lay.native)
                buf.extend(_bytes_of(v, size, lay.big))
                j += 1
        elif code == "f":
            if argi + count > total:
                raise error("pack expected more items for format")
            j = 0
            while j < count:
                bits = _f_bits(_to_float(args[argi]))
                argi += 1
                buf.extend(_bytes_of(bits, 4, lay.big))
                j += 1
        elif code == "d":
            if argi + count > total:
                raise error("pack expected more items for format")
            j = 0
            while j < count:
                hi, lo = _d_bits(_to_float(args[argi]))
                argi += 1
                hi_b = _bytes_of(hi, 4, lay.big)
                lo_b = _bytes_of(lo, 4, lay.big)
                if lay.big:
                    buf.extend(hi_b)
                    buf.extend(lo_b)
                else:
                    buf.extend(lo_b)
                    buf.extend(hi_b)
                j += 1
        elif code == "?":
            j = 0
            while j < count:
                v = _to_int(args[argi])
                argi += 1
                if v != 0:
                    buf.append(1)
                else:
                    buf.append(0)
                j += 1
        elif code == "c":
            j = 0
            while j < count:
                raw = _as_bytes(args[argi])
                argi += 1
                if len(raw) == 0:
                    buf.append(0)
                else:
                    buf.append(raw[0])
                j += 1
        elif code == "s":
            raw = _as_bytes(args[argi])
            argi += 1
            i = 0
            while i < count:
                if i < len(raw):
                    buf.append(raw[i])
                else:
                    buf.append(0)
                i += 1
        elif code == "p":
            raw = _as_bytes(args[argi])
            argi += 1
            limit = count - 1
            n = len(raw)
            if n > limit:
                n = limit
            buf.append(n)
            i = 0
            while i < limit:
                if i < n:
                    buf.append(raw[i])
                else:
                    buf.append(0)
                i += 1
        elif code == "x":
            i = 0
            while i < count:
                buf.append(0)
                i += 1
        _pad_to(buf, off + _item_size(code, lay.native) * count)
    if argi < total:
        raise error("pack expected fewer items for format")
    _pad_to(buf, lay.size)
    return bytes(buf)


# ── 解包 ────────────────────────────────────────────────────────


def _unpack_at(fmt: str, data: bytes, offset: int) -> tuple[unknown, ...]:
    lay = _parse(fmt)
    if offset + lay.size > len(data):
        raise error("unpack requires a buffer of " + str(lay.size) + " bytes")
    vals: list[unknown] = []
    for k in range(len(lay.codes)):
        code = lay.codes[k]
        count = lay.counts[k]
        off = offset + lay.offsets[k]
        size = _item_size(code, lay.native)
        if code in "bBhHiIlLqQ":
            j = 0
            while j < count:
                p = off + j * size
                if code in _SIGNED_CODES:
                    vals.append(_read_int(data, p, size, lay.big))
                else:
                    vals.append(_read_uint(data, p, size, lay.big))
                j += 1
        elif code == "f":
            j = 0
            while j < count:
                p = off + j * 4
                bits = _read_uint(data, p, 4, lay.big)
                vals.append(_bits_f(bits))
                j += 1
        elif code == "d":
            hi = _read_uint(data, off, 4, lay.big)
            lo = _read_uint(data, off + 4, 4, lay.big)
            if not lay.big:
                hi = _read_uint(data, off + 4, 4, False)
                lo = _read_uint(data, off, 4, False)
            vals.append(_bits_d(hi, lo))
        elif code == "?":
            j = 0
            while j < count:
                vals.append(data[off + j] != 0)
                j += 1
        elif code == "c":
            raw = data[off:off + count]
            vals.append(raw)
        elif code == "s":
            vals.append(data[off:off + count])
        elif code == "p":
            n = data[off]
            if n > count - 1:
                n = count - 1
            vals.append(data[off + 1:off + 1 + n])
        elif code == "x":
            pass
    return tuple(vals)


def unpack(fmt: str, data: bytes) -> tuple[unknown, ...]:
    """按格式串解包 bytes，返回元组。"""
    return _unpack_at(fmt, data, 0)


def unpack_from(fmt: str, data: bytes, offset: int = 0) -> tuple[unknown, ...]:
    """从 offset 开始解包。"""
    return _unpack_at(fmt, data, offset)


def iter_unpack(fmt: str, data: bytes) -> list[tuple[unknown, ...]]:
    """按记录大小逐条解包，返回元组列表（CPython 是迭代器）。"""
    lay = _parse(fmt)
    if lay.size <= 0:
        raise error("iter_unpack requires a positive record size")
    out: list[tuple[unknown, ...]] = []
    if len(data) % lay.size != 0:
        raise error("iter_unpack requires a buffer of multiple of " + str(lay.size))
    off = 0
    while off + lay.size <= len(data):
        out.append(_unpack_at(fmt, data, off))
        off = off + lay.size
    return out
