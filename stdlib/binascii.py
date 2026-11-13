"""binascii 模块的 pyadamas 子集（纯 pyadamas 源码）。

支持 hexlify / unhexlify / b2a_hex / a2b_hex / Error。
非十六进制字符抛 Error（ValueError 子类）。
"""


class Error(ValueError):
    pass


_HEX = "0123456789abcdef"
_VAL: list[int] = []
_i = 0
while _i < 256:
    _VAL.append(-1)
    _i += 1
_i = 0
while _i < 10:
    _VAL[48 + _i] = _i
    _i += 1
_i = 0
while _i < 6:
    _VAL[97 + _i] = 10 + _i
    _VAL[65 + _i] = 10 + _i
    _i += 1


def hexlify(data: bytes) -> bytes:
    out: list[int] = []
    for b in data:
        out.append(ord(_HEX[(b >> 4) & 15]))
        out.append(ord(_HEX[b & 15]))
    return bytes(out)


def unhexlify(data: bytes) -> bytes:
    out: list[int] = []
    n = len(data)
    if n % 2 != 0:
        raise Error("Odd-length string")
    i = 0
    while i + 1 < n:
        hi = _VAL[data[i]]
        lo = _VAL[data[i + 1]]
        if hi < 0 or lo < 0:
            raise Error("Non-hexadecimal digit found")
        out.append((hi << 4) | lo)
        i += 2
    return bytes(out)


def b2a_hex(data: bytes) -> bytes:
    return hexlify(data)


def a2b_hex(data: bytes) -> bytes:
    return unhexlify(data)
