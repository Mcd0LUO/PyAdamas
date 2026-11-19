"""zlib 模块的 pyadamas 子集（压缩/解压缩/CRC，系统 zlib 原生实现）。

热路径（compress/decompress/crc32/adler32）直调系统 zlib（cimport zlib，
link z）。gzip 格式的封装在 stdlib/gzip.py。

语义边界：
- compress 的 wbits 不支持（固定 zlib 格式 MAX_WBITS=15）；gzip 格式用
  gzip.compress。level 收 -1..9（-1 是 zlib 默认级别 6）。
- decompress 的 wbits 不支持（只收 zlib 格式）；raw/gzip 用 gzip.decompress。
- 错误类型 zlib.error，消息按常见情形对齐 CPython（-3 incorrect header
  check / -5 incomplete or truncated stream）；其它 zlib msg 细分不保证。
- compressobj/decompressobj 流式对象不做，长尾用 from python import zlib 桥接。
"""

cimport zlib


class error(Exception):
    pass


Z_DEFAULT_COMPRESSION: int = -1
Z_NO_COMPRESSION: int = 0
Z_BEST_SPEED: int = 1
Z_BEST_COMPRESSION: int = 9
MAX_WBITS: int = 15


def crc32(data: bytes, value: int = 0) -> int:
    return zlib.crc32(value, data, len(data))


def adler32(data: bytes, value: int = 1) -> int:
    return zlib.adler32(value, data, len(data))


def compress(data: bytes, level: int = -1) -> bytes:
    if level < -1 or level > 9:
        raise error("Bad compression level")
    n = len(data)
    cap = n + n // 1000 + 64
    out = bytes(cap)
    dl: list[int] = [cap]
    rc = zlib.compress2(out, dl, data, n, level)
    if rc != 0:
        raise error("Error %d while compressing data" % rc)
    return out[:dl[0]]


def decompress(data: bytes, wbits: int = 15) -> bytes:
    if wbits != MAX_WBITS:
        raise error("wbits other than MAX_WBITS is not supported; use gzip.decompress for gzip data")
    n = len(data)
    if n == 0:
        raise error("Error -5 while decompressing data: incomplete or truncated stream")
    cap = n * 4
    if cap < 1024:
        cap = 1024
    while True:
        out = bytes(cap)
        ol: list[int] = [0]
        iu: list[int] = [0]
        rc = zlib.zlib_inflate(data, n, out, cap, ol, iu)
        if rc == 1:
            return out[:ol[0]]
        if rc == 0 and ol[0] == cap and cap < (1 << 34):
            cap = cap * 2
            continue
        if rc == -3:
            raise error("Error -3 while decompressing data: incorrect header check")
        if rc == -5:
            raise error("Error -5 while decompressing data: incomplete or truncated stream")
        raise error("Error %d while decompressing data" % rc)
