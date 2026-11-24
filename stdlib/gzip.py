"""gzip 模块的 pyadamas 子集（系统 zlib 原生压缩，CPython 字节级对齐）。

热路径：raw deflate/inflate 走 C 胶水（stdlib/gzip_glue.c，cimport zlib，
link z）；gzip 头/尾在 Python 侧按 CPython gzip 模块口径组装/校验。
compress 默认 mtime=0（同 CPython 3.11+，可复现）；OS 字节 255、XFL 按
级别（9→2，<2→4，其余 0），与 CPython 逐字节一致。

覆盖：compress / decompress / open / GzipFile（read/readline/readlines/
write/close/flush/tell/readable/writable/__enter__/__exit__/迭代）。

语义边界：
- GzipFile 读是整文件解包、写是 close 时一次压缩（CPython 是流式）；
  内存占用与文件大小成正比，超大文件用桥接。
- open 收 r/rb/w/wb/rt/wt；创建/追加模式（x/xb/a/ab）不做。fileobj 不做。
  二进制模式下 encoding/errors/newline 参数被忽略（CPython 会报错）。
- compress 的 mtime 只收 int（CPython 的 None＝当前时间不做）。
- 文本模式按 utf-8（encoding/errors 可传）；读按通用换行折叠 \r\n/\r。
- 损坏流的 zlib.error 消息细分（invalid distance 等）不保证逐字一致。
- seek 不做；tell 给出读位置/已写字节数。
"""

import zlib as _zlib
cimport zlib as _cz
import _gzipio
from typing import cast


class BadGzipFile(OSError):
    pass


_FTEXT: int = 1
_FHCRC: int = 2
_FEXTRA: int = 4
_FNAME: int = 8
_FCOMMENT: int = 16

_EOF_MSG: str = "Compressed file ended before the end-of-stream marker was reached"


def _le32(n: int) -> bytes:
    return bytes([n & 255, (n >> 8) & 255, (n >> 16) & 255, (n >> 24) & 255])


def _read_le32(data: bytes, pos: int) -> int:
    return data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16) | (data[pos + 3] << 24)


def _bytes_repr(b: bytes) -> str:
    """bytes 的 repr（错误消息用）：可打印 ASCII 原样，其余 \\xNN。"""
    out = "b'"
    i = 0
    while i < len(b):
        c = b[i]
        if c == 39:
            out += "\'"
        elif c == 92:
            out += "\\"
        elif c >= 32 and c < 127:
            out += chr(c)
        else:
            h = hex(c)[2:]
            if len(h) < 2:
                h = "0" + h
            out += "\\x" + h
        i += 1
    return out + "'"


def _deflate(data: bytes, level: int) -> bytes:
    n = len(data)
    cap = n + n // 1000 + 128
    while True:
        out = bytes(cap)
        ol: list[int] = [0]
        rc = _cz.deflate_raw(data, n, out, cap, level, ol)
        if rc >= 0:
            return out[:rc]
        if rc == -1:
            cap = cap * 2
            continue
        if rc == -2:
            raise _zlib.error("Bad compression level")
        raise _zlib.error("Error %d while compressing data" % rc)


def _inflate(src: bytes) -> tuple[bytes, int]:
    """raw inflate；返回 (解出字节, 消耗输入字节)。截断抛 EOFError。"""
    n = len(src)
    cap = n * 4
    if cap < 1024:
        cap = 1024
    while True:
        out = bytes(cap)
        ol: list[int] = [0]
        iu: list[int] = [0]
        rc = _cz.inflate_raw(src, n, out, cap, ol, iu)
        if rc == 1:
            return (out[:ol[0]], iu[0])
        if rc == 0 and ol[0] == cap and cap < (1 << 34):
            cap = cap * 2
            continue
        if rc == -3:
            raise _zlib.error("Error -3 while decompressing data: incorrect header check")
        raise EOFError(_EOF_MSG)


def compress(data: bytes, compresslevel: int = 9, *, mtime: int = 0) -> bytes:
    if compresslevel < 0 or compresslevel > 9:
        raise _zlib.error("Bad compression level")
    xfl = 0
    if compresslevel == 9:
        xfl = 2
    elif compresslevel < 2:
        xfl = 4
    header = b"\x1f\x8b\x08\x00" + _le32(mtime) + bytes([xfl, 255])
    body = _deflate(data, compresslevel)
    crc = _zlib.crc32(data)
    return header + body + _le32(crc) + _le32(len(data) & 0xFFFFFFFF)


def _skip_to_nul(data: bytes, pos: int) -> int:
    while pos < len(data) and data[pos] != 0:
        pos += 1
    if pos < len(data):
        pos += 1
    return pos


def _decompress_members(data: bytes, detailed: bool) -> bytes:
    parts: list[bytes] = []
    while len(data) > 0:
        if len(data) < 2 or data[0] != 0x1F or data[1] != 0x8B:
            raise BadGzipFile("Not a gzipped file (" + _bytes_repr(data[:2]) + ")")
        if len(data) < 10:
            raise EOFError(_EOF_MSG)
        if data[2] != 8:
            raise BadGzipFile("Unknown compression method")
        flag = data[3]
        pos = 10
        if flag & _FEXTRA:
            if pos + 2 > len(data):
                raise EOFError(_EOF_MSG)
            xlen = data[pos] | (data[pos + 1] << 8)
            pos += 2
            if pos + xlen > len(data):
                raise EOFError(_EOF_MSG)
            pos += xlen
        if flag & _FNAME:
            pos = _skip_to_nul(data, pos)
        if flag & _FCOMMENT:
            pos = _skip_to_nul(data, pos)
        if flag & _FHCRC:
            if pos + 2 > len(data):
                raise EOFError(_EOF_MSG)
            pos += 2
        src = data[pos:]
        produced, used = _inflate(src)
        if len(src) - used < 8:
            raise EOFError(_EOF_MSG)
        tpos = pos + used
        crc = _read_le32(data, tpos)
        isize = _read_le32(data, tpos + 4)
        got = _zlib.crc32(produced)
        if crc != got:
            if detailed:
                raise BadGzipFile("CRC check failed %s != %s" % (hex(crc), hex(got)))
            raise BadGzipFile("CRC check failed")
        if isize != (len(produced) & 0xFFFFFFFF):
            raise BadGzipFile("Incorrect length of data produced")
        parts.append(produced)
        data = data[tpos + 8:]
        k = 0
        while k < len(data) and data[k] == 0:
            k += 1
        data = data[k:]
    if len(parts) == 1:
        return parts[0]
    # b"".join 一次分配；此前逐段 `result = result + p` 是二次曲线
    return b"".join(parts)


def decompress(data: bytes) -> bytes:
    return _decompress_members(data, False)


def _universal(s: str) -> str:
    return s.replace("\r\n", "\n").replace("\r", "\n")


class GzipFile:
    """gzip 文件句柄（整读整写口径，见模块文档）。"""
    name: str
    closed: bool
    _mode: str
    _text: bool
    _encoding: str
    _errors: str
    _level: int
    _mtime: int
    _rbuf: bytes
    _rpos: int
    _loaded: bool
    _wparts: list[bytes]
    _wlen: int

    def __init__(self, filename: str, mode: str = "rb", compresslevel: int = 9,
                 mtime: int = 0, encoding: str = "utf-8", errors: str = "strict") -> None:
        m = mode
        if m == "r":
            m = "rb"
        elif m == "w":
            m = "wb"
        if m != "rb" and m != "wb":
            raise ValueError("Invalid mode: '" + mode + "'")
        self.name = filename
        self.closed = False
        self._mode = m
        self._text = False
        self._encoding = encoding
        self._errors = errors
        self._level = compresslevel
        self._mtime = mtime
        self._rbuf = b""
        self._rpos = 0
        self._loaded = False
        self._wparts = []
        self._wlen = 0

    def _load(self) -> None:
        if self._loaded:
            return
        raw = _gzipio.read_bin(self.name)
        self._rbuf = _decompress_members(raw, True)
        self._loaded = True

    def readable(self) -> bool:
        return self._mode == "rb"

    def writable(self) -> bool:
        return self._mode == "wb"

    def read(self, size: int = -1) -> object:
        if self.closed:
            raise ValueError("I/O operation on closed file")
        if not self.readable():
            raise OSError("[Errno 9] read() on write-only GzipFile object")
        self._load()
        n = len(self._rbuf)
        end = n
        if size >= 0 and self._rpos + size < n:
            end = self._rpos + size
        chunk = self._rbuf[self._rpos:end]
        self._rpos = end
        if self._text:
            return _universal(chunk.decode(self._encoding, self._errors))
        return chunk

    def readline(self, size: int = -1) -> object:
        if self.closed:
            raise ValueError("I/O operation on closed file")
        if not self.readable():
            raise OSError("[Errno 9] readline() on write-only GzipFile object")
        self._load()
        n = len(self._rbuf)
        if self._rpos >= n:
            if self._text:
                return ""
            return b""
        nl = self._rbuf.find(b"\n", self._rpos)
        end = n
        if nl >= 0:
            end = nl + 1
        if size >= 0 and self._rpos + size < end:
            end = self._rpos + size
        chunk = self._rbuf[self._rpos:end]
        self._rpos = end
        if self._text:
            return _universal(chunk.decode(self._encoding, self._errors))
        return chunk

    def readlines(self) -> list[object]:
        out: list[object] = []
        while True:
            line = self.readline()
            if self._text:
                s: str = cast(str, line)
                if len(s) == 0:
                    break
            else:
                b: bytes = cast(bytes, line)
                if len(b) == 0:
                    break
            out.append(line)
        return out

    def __iter__(self) -> list[object]:
        return self.readlines()

    def write(self, data: object) -> int:
        if self.closed:
            raise ValueError("I/O operation on closed file")
        if not self.writable():
            raise OSError("[Errno 9] write() on read-only GzipFile object")
        if self._text:
            s: str = cast(str, data)
            b = s.encode(self._encoding, self._errors)
            self._wparts.append(b)
            self._wlen += len(b)
            return len(s)
        b2: bytes = cast(bytes, data)
        self._wparts.append(b2)
        self._wlen += len(b2)
        return len(b2)

    def tell(self) -> int:
        if self.readable():
            return self._rpos
        return self._wlen

    def flush(self) -> None:
        pass

    def close(self) -> None:
        if self.closed:
            return
        self.closed = True
        if self.writable():
            payload = b""
            for p in self._wparts:
                payload = payload + p
            comp = compress(payload, self._level, mtime=self._mtime)
            _gzipio.write_bin(self.name, comp)

    def __enter__(self) -> "GzipFile":
        return self

    def __exit__(self, exc_type: object, exc_val: object, exc_tb: object) -> bool:
        self.close()
        return False


def open(filename: str, mode: str = "rb", compresslevel: int = 9,
         encoding: str = "utf-8", errors: str = "strict", newline: str = "") -> GzipFile:
    if "t" in mode and "b" in mode:
        raise ValueError("Invalid mode: '" + mode + "'")
    ncreate = 0
    if "r" in mode:
        ncreate += 1
    if "w" in mode:
        ncreate += 1
    if "x" in mode:
        ncreate += 1
    if "a" in mode:
        ncreate += 1
    if ncreate != 1:
        raise ValueError("must have exactly one of create/read/write/append mode")
    gz = mode.replace("t", "")
    text = "t" in mode
    if gz != "r" and gz != "rb" and gz != "w" and gz != "wb":
        raise ValueError("gzip mode '" + mode + "' is not supported yet (use r/rb/w/wb/rt/wt)")
    f = GzipFile(filename, gz, compresslevel, 0, encoding, errors)
    f._text = text
    return f
