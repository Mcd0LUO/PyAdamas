"""hashlib 模块的 pyadamas 子集（纯 pyadamas 源码，随编译器分发）。

覆盖 md5 / sha1 / sha256 的 update / digest / hexdigest。摘要按字节计算，
与 CPython 结果一致；不建模 usedforsecurity= / copy() / 增量分块的性能优化。
"""

_K_MD5 = [
    3614090360, 3905402710, 606105819, 3250441966,
    4118548399, 1200080426, 2821735955, 4249261313,
    1770035416, 2336552879, 4294925233, 2304563134,
    1804603682, 4254626195, 2792965006, 1236535329,
    4129170786, 3225465664, 643717713, 3921069994,
    3593408605, 38016083, 3634488961, 3889429448,
    568446438, 3275163606, 4107603335, 1163531501,
    2850285829, 4243563512, 1735328473, 2368359562,
    4294588738, 2272392833, 1839030562, 4259657740,
    2763975236, 1272893353, 4139469664, 3200236656,
    681279174, 3936430074, 3572445317, 76029189,
    3654602809, 3873151461, 530742520, 3299628645,
    4096336452, 1126891415, 2878612391, 4237533241,
    1700485571, 2399980690, 4293915773, 2240044497,
    1873313359, 4264355552, 2734768916, 1309151649,
    4149444226, 3174756917, 718787259, 3951481745,
]
_S_MD5 = [
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
]


def _rotl(x: int, s: int) -> int:
    x = x & 4294967295
    return ((x << s) | (x >> (32 - s))) & 4294967295


def _rotr(x: int, s: int) -> int:
    x = x & 4294967295
    return ((x >> s) | (x << (32 - s))) & 4294967295


def _le32(buf: list[int], off: int) -> int:
    return buf[off] | (buf[off + 1] << 8) | (buf[off + 2] << 16) | (buf[off + 3] << 24)


def _be32(buf: list[int], off: int) -> int:
    return (buf[off] << 24) | (buf[off + 1] << 16) | (buf[off + 2] << 8) | buf[off + 3]


def _le_words(hs: list[int]) -> bytes:
    out: list[int] = []
    for h in hs:
        j = 0
        while j < 4:
            out.append((h >> (8 * j)) & 255)
            j += 1
    return bytes(out)


def _be_words(hs: list[int]) -> bytes:
    out: list[int] = []
    for h in hs:
        j = 3
        while j >= 0:
            out.append((h >> (8 * j)) & 255)
            j -= 1
    return bytes(out)


def _pad(data: list[int], nbytes: int, little: bool) -> list[int]:
    """追加 0x80 与长度字段；MD5/SHA-1/SHA-256 的补齐规则相同。"""
    msg: list[int] = []
    for b in data:
        msg.append(b)
    bitlen = nbytes * 8
    msg.append(128)
    while len(msg) % 64 != 56:
        msg.append(0)
    i = 0
    if little:
        while i < 8:
            msg.append((bitlen >> (8 * i)) & 255)
            i += 1
    else:
        i = 7
        while i >= 0:
            msg.append((bitlen >> (8 * i)) & 255)
            i -= 1
    return msg


def _hexdigest(digest: bytes) -> str:
    chars = "0123456789abcdef"
    out = ""
    for b in digest:
        out = out + chars[b >> 4] + chars[b & 15]
    return out


class MD5:
    def __init__(self, data: bytes = b"") -> None:
        self.h0 = 1732584193
        self.h1 = 4023233417
        self.h2 = 2562383102
        self.h3 = 271733878
        self.data = []
        self.nbytes = 0
        self.update(data)

    def update(self, data: bytes) -> None:
        for b in data:
            self.data.append(b)
        self.nbytes += len(data)

    def digest(self) -> bytes:
        msg = _pad(self.data, self.nbytes, True)
        a0 = self.h0
        b0 = self.h1
        c0 = self.h2
        d0 = self.h3
        off = 0
        while off < len(msg):
            m: list[int] = []
            j = 0
            while j < 16:
                m.append(_le32(msg, off + j * 4))
                j += 1
            a = a0
            b = b0
            c = c0
            d = d0
            i = 0
            while i < 64:
                f = 0
                g = 0
                if i < 16:
                    f = (b & c) | ((~b) & d)
                    g = i
                elif i < 32:
                    f = (d & b) | ((~d) & c)
                    g = (5 * i + 1) % 16
                elif i < 48:
                    f = b ^ c ^ d
                    g = (3 * i + 5) % 16
                else:
                    f = c ^ (b | (~d))
                    g = (7 * i) % 16
                f = f & 4294967295
                tmp = d
                d = c
                c = b
                b = (b + _rotl((a + f + _K_MD5[i] + m[g]) & 4294967295, _S_MD5[i])) & 4294967295
                a = tmp
                i += 1
            a0 = (a0 + a) & 4294967295
            b0 = (b0 + b) & 4294967295
            c0 = (c0 + c) & 4294967295
            d0 = (d0 + d) & 4294967295
            off += 64
        return _le_words([a0, b0, c0, d0])

    def hexdigest(self) -> str:
        return _hexdigest(self.digest())


def md5(data: bytes = b"") -> MD5:
    return MD5(data)


_K_SHA256 = [
    1116352408, 1899447441, 3049323471, 3921009573,
    961987163, 1508970993, 2453635748, 2870763221,
    3624381080, 310598401, 607225278, 1426881987,
    1925078388, 2162078206, 2614888103, 3248222580,
    3835390401, 4022224774, 264347078, 604807628,
    770255983, 1249150122, 1555081692, 1996064986,
    2554220882, 2821834349, 2952996808, 3210313671,
    3336571891, 3584528711, 113926993, 338241895,
    666307205, 773529912, 1294757372, 1396182291,
    1695183700, 1986661051, 2177026350, 2456956037,
    2730485921, 2820302411, 3259730800, 3345764771,
    3516065817, 3600352804, 4094571909, 275423344,
    430227734, 506948616, 659060556, 883997877,
    958139571, 1322822218, 1537002063, 1747873779,
    1955562222, 2024104815, 2227730452, 2361852424,
    2428436474, 2756734187, 3204031479, 3329325298,
]


class SHA1:
    def __init__(self, data: bytes = b"") -> None:
        self.h0 = 1732584193
        self.h1 = 4023233417
        self.h2 = 2562383102
        self.h3 = 271733878
        self.h4 = 3285377520
        self.data = []
        self.nbytes = 0
        self.update(data)

    def update(self, data: bytes) -> None:
        for b in data:
            self.data.append(b)
        self.nbytes += len(data)

    def digest(self) -> bytes:
        msg = _pad(self.data, self.nbytes, False)
        h0 = self.h0
        h1 = self.h1
        h2 = self.h2
        h3 = self.h3
        h4 = self.h4
        off = 0
        while off < len(msg):
            w: list[int] = []
            j = 0
            while j < 16:
                w.append(_be32(msg, off + j * 4))
                j += 1
            while j < 80:
                w.append(_rotl(w[j - 3] ^ w[j - 8] ^ w[j - 14] ^ w[j - 16], 1))
                j += 1
            a = h0
            b = h1
            c = h2
            d = h3
            e = h4
            i = 0
            while i < 80:
                f = 0
                k = 0
                if i < 20:
                    f = (b & c) | ((~b) & d)
                    k = 1518500249
                elif i < 40:
                    f = b ^ c ^ d
                    k = 1859775393
                elif i < 60:
                    f = (b & c) | (b & d) | (c & d)
                    k = 2400959708
                else:
                    f = b ^ c ^ d
                    k = 3395469782
                f = f & 4294967295
                tmp = (_rotl(a, 5) + f + e + k + w[i]) & 4294967295
                e = d
                d = c
                c = _rotl(b, 30)
                b = a
                a = tmp
                i += 1
            h0 = (h0 + a) & 4294967295
            h1 = (h1 + b) & 4294967295
            h2 = (h2 + c) & 4294967295
            h3 = (h3 + d) & 4294967295
            h4 = (h4 + e) & 4294967295
            off += 64
        return _be_words([h0, h1, h2, h3, h4])

    def hexdigest(self) -> str:
        return _hexdigest(self.digest())


class SHA256:
    def __init__(self, data: bytes = b"") -> None:
        self.h = [
            1779033703, 3144134277, 1013904242, 2773480762,
            1359893119, 2600822924, 528734635, 1541459225,
        ]
        self.data = []
        self.nbytes = 0
        self.update(data)

    def update(self, data: bytes) -> None:
        for b in data:
            self.data.append(b)
        self.nbytes += len(data)

    def digest(self) -> bytes:
        msg = _pad(self.data, self.nbytes, False)
        h = []
        for x in self.h:
            h.append(x)
        off = 0
        while off < len(msg):
            w: list[int] = []
            j = 0
            while j < 16:
                w.append(_be32(msg, off + j * 4))
                j += 1
            while j < 64:
                x = w[j - 15]
                y = w[j - 2]
                s0 = _rotr(x, 7) ^ _rotr(x, 18) ^ (x >> 3)
                s1 = _rotr(y, 17) ^ _rotr(y, 19) ^ (y >> 10)
                w.append((w[j - 16] + s0 + w[j - 7] + s1) & 4294967295)
                j += 1
            a = h[0]
            b = h[1]
            c = h[2]
            d = h[3]
            e = h[4]
            f = h[5]
            g = h[6]
            hh = h[7]
            i = 0
            while i < 64:
                s1 = _rotr(e, 6) ^ _rotr(e, 11) ^ _rotr(e, 25)
                ch = (e & f) ^ ((~e) & g)
                t1 = (hh + s1 + ch + _K_SHA256[i] + w[i]) & 4294967295
                s0 = _rotr(a, 2) ^ _rotr(a, 13) ^ _rotr(a, 22)
                maj = (a & b) ^ (a & c) ^ (b & c)
                t2 = (s0 + maj) & 4294967295
                hh = g
                g = f
                f = e
                e = (d + t1) & 4294967295
                d = c
                c = b
                b = a
                a = (t1 + t2) & 4294967295
                i += 1
            h[0] = (h[0] + a) & 4294967295
            h[1] = (h[1] + b) & 4294967295
            h[2] = (h[2] + c) & 4294967295
            h[3] = (h[3] + d) & 4294967295
            h[4] = (h[4] + e) & 4294967295
            h[5] = (h[5] + f) & 4294967295
            h[6] = (h[6] + g) & 4294967295
            h[7] = (h[7] + hh) & 4294967295
            off += 64
        return _be_words(h)

    def hexdigest(self) -> str:
        return _hexdigest(self.digest())


def sha1(data: bytes = b"") -> SHA1:
    return SHA1(data)


def sha256(data: bytes = b"") -> SHA256:
    return SHA256(data)
