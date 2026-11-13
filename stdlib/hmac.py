"""hmac 模块的 pyadamas 子集（纯 pyadamas 源码，随编译器分发）。

覆盖 HMAC-MD5 / HMAC-SHA1 / HMAC-SHA256：
`new(key, msg=b"", digestmod="sha256")` 返回带 update / digest / hexdigest
的对象。digestmod 用字符串名（与 CPython 的 hmac.new(key, msg, "sha256")
一致），不支持传入 hash 构造函数或模块对象。
"""

import hashlib


def _hash(name: str, data: bytes) -> bytes:
    if name == "md5":
        h = hashlib.md5(data)
        return h.digest()
    if name == "sha1":
        h = hashlib.sha1(data)
        return h.digest()
    if name == "sha256":
        h = hashlib.sha256(data)
        return h.digest()
    raise ValueError("unsupported digestmod")


class HMAC:
    def __init__(self, key: bytes, msg: bytes = b"", digestmod: str = "sha256") -> None:
        self.key_bytes = []
        for b in key:
            self.key_bytes.append(b)
        self.data = []
        for b in msg:
            self.data.append(b)
        self.digestmod = digestmod

    def update(self, msg: bytes) -> None:
        for b in msg:
            self.data.append(b)

    def _normalized_key(self) -> bytes:
        k = bytes(self.key_bytes)
        if len(k) > 64:
            k = _hash(self.digestmod, k)
        out: list[int] = []
        for b in k:
            out.append(b)
        while len(out) < 64:
            out.append(0)
        return bytes(out)

    def digest(self) -> bytes:
        k = self._normalized_key()
        ipad: list[int] = []
        opad: list[int] = []
        for b in k:
            ipad.append(b ^ 54)
            opad.append(b ^ 92)
        inner_data: list[int] = []
        for b in ipad:
            inner_data.append(b)
        for b in self.data:
            inner_data.append(b)
        inner = _hash(self.digestmod, bytes(inner_data))
        outer_data: list[int] = []
        for b in opad:
            outer_data.append(b)
        for b in inner:
            outer_data.append(b)
        return _hash(self.digestmod, bytes(outer_data))

    def hexdigest(self) -> str:
        d = self.digest()
        chars = "0123456789abcdef"
        out = ""
        for b in d:
            out = out + chars[b >> 4] + chars[b & 15]
        return out


def new(key: bytes, msg: bytes = b"", digestmod: str = "sha256") -> HMAC:
    return HMAC(key, msg, digestmod)
