"""base64 模块的 pyadamas 子集（纯 pyadamas 源码，随编译器分发）。

覆盖 b64encode / b64decode / urlsafe_b64encode / urlsafe_b64decode /
standard_b64encode / standard_b64decode / b16encode / b16decode /
b32encode / b32decode / encodebytes / decodebytes。
不建模 validate= / altchars= 与 str 输入；解码同时接受标准与 URL-safe 字母表，
非法字符抛 ValueError。
"""

_B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
_URL = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
_B32 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"
_HEX = "0123456789abcdef"

_b32_lookup: list[int] = []
_i = 0
while _i < 256:
    _b32_lookup.append(-1)
    _i += 1
_i = 0
while _i < 32:
    _b32_lookup[ord(_B32[_i])] = _i
    _i += 1

_hex_lookup: list[int] = []
_i = 0
while _i < 256:
    _hex_lookup.append(-1)
    _i += 1
_i = 0
while _i < 10:
    _hex_lookup[48 + _i] = _i
    _i += 1
_i = 0
while _i < 6:
    _hex_lookup[97 + _i] = 10 + _i
    _hex_lookup[65 + _i] = 10 + _i
    _i += 1


# b64 编解码走 C intrinsic（__pa_b64encode / __pa_b64decode），
# 纯 PyAdamas 的逐字节版本比 CPython 慢，这里只保留公开入口。


def b64encode(data: bytes) -> bytes:
    return __pa_b64encode(data, 0)


def urlsafe_b64encode(data: bytes) -> bytes:
    return __pa_b64encode(data, 1)


def b64decode(data: bytes) -> bytes:
    return __pa_b64decode(data)


def standard_b64encode(data: bytes) -> bytes:
    return __pa_b64encode(data, 0)


def standard_b64decode(data: bytes) -> bytes:
    return __pa_b64decode(data)


def b16encode(data: bytes) -> bytes:
    out: list[int] = []
    for b in data:
        out.append(ord(_HEX[(b >> 4) & 15]))
        out.append(ord(_HEX[b & 15]))
    return bytes(out)


def b16decode(data: bytes) -> bytes:
    out: list[int] = []
    n = len(data)
    if n % 2 != 0:
        # CPython b16decode 先校验字符集再校验长度，任何奇数长度输入
        # 都报 Non-base16 digit found（binascii.unhexlify 才是 Odd-length）。
        raise ValueError("Non-base16 digit found")
    i = 0
    while i + 1 < n:
        hi = _hex_lookup[data[i]]
        lo = _hex_lookup[data[i + 1]]
        if hi < 0 or lo < 0:
            raise ValueError("Non-base16 digit found")
        out.append((hi << 4) | lo)
        i += 2
    return bytes(out)


def b32encode(data: bytes) -> bytes:
    out: list[int] = []
    buf = 0
    bits = 0
    for b in data:
        buf = (buf << 8) | b
        bits += 8
        while bits >= 5:
            bits -= 5
            out.append(ord(_B32[(buf >> bits) & 31]))
    if bits > 0:
        out.append(ord(_B32[(buf << (5 - bits)) & 31]))
    while len(out) % 8 != 0:
        out.append(61)
    return bytes(out)


def b32decode(data: bytes) -> bytes:
    out: list[int] = []
    buf = 0
    bits = 0
    for ch in data:
        if ch == 61:
            continue
        v = _b32_lookup[ch]
        if v < 0:
            raise ValueError("invalid base32 character")
        buf = (buf << 5) | v
        bits += 5
        if bits >= 8:
            bits -= 8
            out.append((buf >> bits) & 255)
            buf = buf & ((1 << bits) - 1)
    return bytes(out)


def encodebytes(data: bytes) -> bytes:
    enc = b64encode(data)
    out: list[int] = []
    i = 0
    while i < len(enc):
        chunk = enc[i:i + 76]
        for ch in chunk:
            out.append(ch)
        out.append(10)
        i += 76
    return bytes(out)


def decodebytes(data: bytes) -> bytes:
    out: list[int] = []
    for ch in data:
        if ch == 10 or ch == 13:
            continue
        out.append(ch)
    return b64decode(bytes(out))


def urlsafe_b64decode(data: bytes) -> bytes:
    return __pa_b64decode(data)
