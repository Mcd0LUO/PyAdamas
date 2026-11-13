"""base64 模块的 pyadamas 子集（纯 pyadamas 源码，随编译器分发）。

覆盖 b64encode / b64decode / urlsafe_b64encode / urlsafe_b64decode。
不建模 validate= / altchars= 与 str 输入；解码同时接受标准与 URL-safe 字母表，
非法字符抛 ValueError。
"""

_B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
_URL = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"

_lookup: list[int] = []
_i = 0
while _i < 256:
    _lookup.append(-1)
    _i += 1
_i = 0
while _i < 64:
    _lookup[ord(_B64[_i])] = _i
    _lookup[ord(_URL[_i])] = _i
    _i += 1


def _encode(data: bytes, alphabet: str) -> bytes:
    out: list[int] = []
    n = len(data)
    i = 0
    while i + 2 < n:
        a = data[i]
        b = data[i + 1]
        c = data[i + 2]
        out.append(ord(alphabet[(a >> 2) & 63]))
        out.append(ord(alphabet[((a & 3) << 4) | (b >> 4)]))
        out.append(ord(alphabet[((b & 15) << 2) | (c >> 6)]))
        out.append(ord(alphabet[c & 63]))
        i += 3
    left = n - i
    if left == 1:
        a = data[i]
        out.append(ord(alphabet[(a >> 2) & 63]))
        out.append(ord(alphabet[(a & 3) << 4]))
        out.append(61)
        out.append(61)
    elif left == 2:
        a = data[i]
        b = data[i + 1]
        out.append(ord(alphabet[(a >> 2) & 63]))
        out.append(ord(alphabet[((a & 3) << 4) | (b >> 4)]))
        out.append(ord(alphabet[(b & 15) << 2]))
        out.append(61)
    return bytes(out)


def _decode(data: bytes) -> bytes:
    out: list[int] = []
    acc = 0
    bits = 0
    for ch in data:
        if ch == 61:  # '='
            continue
        v = _lookup[ch]
        if v < 0:
            raise ValueError("invalid base64 character")
        acc = (acc << 6) | v
        bits += 6
        if bits >= 8:
            bits -= 8
            out.append((acc >> bits) & 255)
            acc = acc & ((1 << bits) - 1)
    return bytes(out)


def b64encode(data: bytes) -> bytes:
    return _encode(data, _B64)


def urlsafe_b64encode(data: bytes) -> bytes:
    return _encode(data, _URL)


def b64decode(data: bytes) -> bytes:
    return _decode(data)


def urlsafe_b64decode(data: bytes) -> bytes:
    return _decode(data)
