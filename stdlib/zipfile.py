"""zipfile 模块的 pyadamas 子集（纯 pyadamas 源码 + 一批 zlib 胶水）。

读写 ZIP 归档：ZIP_STORED（不压缩）与 ZIP_DEFLATED（raw deflate，复用
stdlib/gzip_glue.c 的 cimport zlib 胶水）。格式按 PKZIP APPNOTE 组装/解析，
合法可读可解压（CPython/unzip 互操作），不追求字节级 header 一致。

覆盖：ZipFile（r/w/a/x、namelist/infolist/getinfo/read/write/writestr/
extract/extractall/testzip/close/comment/filename、上下文管理、迭代、in）、
ZipInfo（常用字段 + is_dir/from_file）、ZIP_STORED/ZIP_DEFLATED 常量、
BadZipFile/LargeZipFile。

语义边界（轻量标尺，见 docs/stdlib-batch2.md）：
- 整读整写（同一批 GzipFile 口径）：内存与归档大小成正比；流式/超大归档
  用 from python import zipfile 桥接。
- Zip64 不支持：读遇到 Zip64 结构抛 NotImplementedError；写超出 32 位
  容量/65535 条目抛 LargeZipFile（allowZip64 参数收下但不生效）。
- 加密不支持：读加密条目抛 RuntimeError（与 CPython 无密码同型）；
  setpassword 收下但解密不做。
- ZIP_BZIP2/ZIP_LZMA 抛 NotImplementedError（系统库未绑）。
- write()/from_file 的 date_time 固定 (1980,1,1,0,0,0)（无 localtime）；
  external_attr 用固定 0o644/0o755（无 stat 权限位）。
- 文件名按 utf-8 解码（非 UTF-8 遗留名用 replace，不做 cp437）。
- fileobj 不做（只收路径 str）；pwd 参数收下但不解密。
- 错误消息合理即可，不逐字对齐 CPython。
"""

import zlib as _zlib
cimport zlib as _cz
import _gzipio
import os
import os.path as _osp
from typing import cast


ZIP_STORED: int = 0
ZIP_DEFLATED: int = 8
ZIP_BZIP2: int = 12
ZIP_LZMA: int = 14

_MAX_COMMENT: int = 65535
_U32MAX: int = 0xFFFFFFFF


class BadZipFile(Exception):
    pass


class LargeZipFile(Exception):
    pass


def _u16(b: bytes, p: int) -> int:
    return b[p] | (b[p + 1] << 8)


def _u32(b: bytes, p: int) -> int:
    return b[p] | (b[p + 1] << 8) | (b[p + 2] << 16) | (b[p + 3] << 24)


def _p16(n: int) -> bytes:
    return bytes([n & 255, (n >> 8) & 255])


def _p32(n: int) -> bytes:
    return bytes([n & 255, (n >> 8) & 255, (n >> 16) & 255, (n >> 24) & 255])


def _dos_time(dt: tuple[int, int, int, int, int, int]) -> tuple[int, int]:
    y = dt[0]
    if y < 1980:
        y = 1980
    if y > 2107:
        y = 2107
    t = (dt[3] << 11) | (dt[4] << 5) | (dt[5] // 2)
    d = ((y - 1980) << 9) | (dt[1] << 5) | dt[2]
    return (t, d)


def _from_dos(t: int, d: int) -> tuple[int, int, int, int, int, int]:
    y = 1980 + ((d >> 9) & 127)
    mo = (d >> 5) & 15
    day = d & 31
    h = (t >> 11) & 31
    mi = (t >> 5) & 63
    s = (t & 31) * 2
    if mo < 1:
        mo = 1
    if mo > 12:
        mo = 12
    if day < 1:
        day = 1
    return (y, mo, day, h, mi, s)


def _deflate(data: bytes, level: int) -> bytes:
    n = len_bytes(data)
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
        raise _zlib.error("Error %d while compressing data" % rc)


def _inflate(comp: bytes, usize: int) -> bytes:
    n = len(comp)
    cap = usize
    if cap < 1024:
        cap = 1024
    if cap > 1048576:
        cap = 1048576
    while True:
        out = bytes(cap)
        ol: list[int] = [0]
        iu: list[int] = [0]
        rc = _cz.inflate_raw(comp, n, out, cap, ol, iu)
        if rc == 1:
            return out[:ol[0]]
        if rc == 0 and ol[0] == cap and cap < (1 << 34):
            cap = cap * 2
            continue
        if rc == -5:
            raise BadZipFile("Compressed data ended before the end-of-stream marker")
        raise BadZipFile("Error %d while decompressing data" % rc)


class ZipInfo:
    """归档条目元数据（常用字段面，见模块文档）。"""
    filename: str
    date_time: tuple[int, int, int, int, int, int]
    compress_type: int
    comment: bytes
    extra: bytes
    create_system: int
    create_version: int
    extract_version: int
    flag_bits: int
    volume: int
    internal_attr: int
    external_attr: int
    header_offset: int
    CRC: int
    compress_size: int
    file_size: int

    def __init__(self, filename: str = "NoName", date_time: object = None) -> None:
        self.filename = filename
        self.date_time = (1980, 1, 1, 0, 0, 0)
        if not (date_time is None):
            a: list[int] = list(date_time)
            self.date_time = (a[0], a[1], a[2], a[3], a[4], a[5])
        self.compress_type = ZIP_STORED
        self.comment = b""
        self.extra = b""
        self.create_system = 0
        self.create_version = 20
        self.extract_version = 20
        self.flag_bits = 0
        self.volume = 0
        self.internal_attr = 0
        self.external_attr = 0
        self.header_offset = 0
        self.CRC = 0
        self.compress_size = 0
        self.file_size = 0

    def is_dir(self) -> bool:
        return self.filename.endswith("/")

    def __repr__(self) -> str:
        return "<ZipInfo filename=" + repr(self.filename) + " compress_type=" + str(self.compress_type) + ">"

    @classmethod
    def from_file(cls, filename: str, arcname: object = None,
                  *, strict_timestamps: bool = True) -> "ZipInfo":
        an = filename
        if not (arcname is None):
            an = arcname
        an = _osp.normpath(an)
        while an.startswith("/"):
            an = an[1:]
        zi = cls(an)
        zi.create_system = 3
        if _osp.isdir(filename):
            if not an.endswith("/"):
                an = an + "/"
            zi.filename = an
            zi.external_attr = (0o40755 << 16) | 0x10
            zi.file_size = 0
            zi.compress_size = 0
            zi.CRC = 0
        else:
            zi.file_size = _osp.getsize(filename)
            zi.external_attr = 0o644 << 16
        return zi


def _find_eocd(data: bytes) -> int:
    n = len(data)
    if n < 22:
        raise BadZipFile("File is not a zip file")
    lo = n - 22 - _MAX_COMMENT
    if lo < 0:
        lo = 0
    i = n - 22
    while i >= lo:
        if data[i] == 0x50 and data[i + 1] == 0x4B and data[i + 2] == 0x05 and data[i + 3] == 0x06:
            clen = _u16(data, i + 20)
            if i + 22 + clen == n:
                return i
        i -= 1
    raise BadZipFile("File is not a zip file")


class ZipFile:
    """ZIP 归档句柄（整读整写口径，见模块文档）。"""
    filename: str
    comment: bytes
    closed: bool
    _mode: str
    _compression: int
    _compresslevel: int
    _entries: list[ZipInfo]
    _payloads: list[bytes]
    _raw: bytes
    _concat: int
    _pwd: object

    def __init__(self, file: str, mode: str = "r", compression: int = ZIP_STORED,
                 allowZip64: bool = True, compresslevel: int = -1, *,
                 strict_timestamps: bool = True) -> None:
        if mode != "r" and mode != "w" and mode != "a" and mode != "x":
            raise ValueError("ZipFile() requires mode 'r', 'w', 'x', or 'a'")
        if compression != ZIP_STORED and compression != ZIP_DEFLATED and \
                compression != ZIP_BZIP2 and compression != ZIP_LZMA:
            raise NotImplementedError("compression type %d is not supported" % compression)
        if compresslevel < -1 or compresslevel > 9:
            raise ValueError("compresslevel must be between -1 and 9")
        self.filename = file
        self.comment = b""
        self.closed = False
        self._mode = mode
        self._compression = compression
        self._compresslevel = compresslevel
        self._entries = []
        self._payloads = []
        self._raw = b""
        self._concat = 0
        self._pwd = None
        if mode == "r" or mode == "a":
            try:
                self._raw = _gzipio.read_bin(file)
            except FileNotFoundError:
                if mode == "r":
                    raise
            if len(self._raw) > 0:
                self._load_central()
            elif mode == "r":
                raise BadZipFile("File is not a zip file")
        if mode == "w" or mode == "x":
            f = open(file, "xb" if mode == "x" else "wb")
            f.close()

    def _load_central(self) -> None:
        data = self._raw
        eocd = _find_eocd(data)
        total = _u16(data, eocd + 10)
        cd_size = _u32(data, eocd + 12)
        cd_off = _u32(data, eocd + 16)
        if total == 0xFFFF or cd_size == _U32MAX or cd_off == _U32MAX:
            raise NotImplementedError("Zip64 archives are not supported")
        self._concat = eocd - cd_size - cd_off
        if self._concat < 0:
            raise BadZipFile("File is not a zip file")
        clen = _u16(data, eocd + 20)
        self.comment = data[eocd + 22:eocd + 22 + clen]
        pos = cd_off + self._concat
        i = 0
        while i < total:
            if data[pos:pos + 4] != b"PK\x01\x02":
                raise BadZipFile("Bad magic number for central directory")
            made_by = _u16(data, pos + 4)
            zi = ZipInfo()
            zi.create_version = made_by & 255
            zi.create_system = made_by >> 8
            zi.extract_version = _u16(data, pos + 6)
            zi.flag_bits = _u16(data, pos + 8)
            zi.compress_type = _u16(data, pos + 10)
            t = _u16(data, pos + 12)
            d = _u16(data, pos + 14)
            zi.date_time = _from_dos(t, d)
            zi.CRC = _u32(data, pos + 16)
            zi.compress_size = _u32(data, pos + 20)
            zi.file_size = _u32(data, pos + 24)
            nlen = _u16(data, pos + 28)
            elen = _u16(data, pos + 30)
            clen2 = _u16(data, pos + 32)
            zi.volume = _u16(data, pos + 34)
            zi.internal_attr = _u16(data, pos + 36)
            zi.external_attr = _u32(data, pos + 38)
            zi.header_offset = _u32(data, pos + 42)
            nb = data[pos + 46:pos + 46 + nlen]
            zi.filename = nb.decode("utf-8", "replace")
            zi.extra = data[pos + 46 + nlen:pos + 46 + nlen + elen]
            zi.comment = data[pos + 46 + nlen + elen:pos + 46 + nlen + elen + clen2]
            if zi.compress_size == _U32MAX or zi.file_size == _U32MAX or zi.header_offset == _U32MAX:
                raise NotImplementedError("Zip64 entries are not supported")
            self._entries.append(zi)
            if self._mode == "a":
                off = zi.header_offset + self._concat
                if data[off:off + 4] != b"PK\x03\x04":
                    raise BadZipFile("Bad magic number for file header")
                lnlen = _u16(data, off + 26)
                lelen = _u16(data, off + 28)
                ds = off + 30 + lnlen + lelen
                self._payloads.append(data[ds:ds + zi.compress_size])
            pos += 46 + nlen + elen + clen2
            i += 1

    def namelist(self) -> list[str]:
        out: list[str] = []
        for zi in self._entries:
            out.append(zi.filename)
        return out

    def infolist(self) -> list[ZipInfo]:
        return list(self._entries)

    def getinfo(self, name: str) -> ZipInfo:
        i = len(self._entries) - 1
        while i >= 0:
            if self._entries[i].filename == name:
                return self._entries[i]
            i -= 1
        raise KeyError("There is no item named " + repr(name) + " in the archive")

    def setpassword(self, pwd: object) -> None:
        self._pwd = pwd

    def _resolve(self, name: object) -> ZipInfo:
        if isinstance(name, ZipInfo):
            return name
        s: str = name
        return self.getinfo(s)

    def read(self, name: object, pwd: object = None) -> bytes:
        if self.closed:
            raise ValueError("read() on closed ZipFile")
        zi = self._resolve(name)
        if self._mode == "w" or self._mode == "x":
            raise ValueError("read() on write-only ZipFile")
        if zi.flag_bits & 0x1:
            raise RuntimeError("File " + repr(zi.filename) + " is encrypted, password required for extraction")
        data = self._raw
        off = zi.header_offset + self._concat
        if data[off:off + 4] != b"PK\x03\x04":
            raise BadZipFile("Bad magic number for file header")
        nlen = _u16(data, off + 26)
        elen = _u16(data, off + 28)
        ds = off + 30 + nlen + elen
        comp = data[ds:ds + zi.compress_size]
        if zi.compress_type == ZIP_STORED:
            out = comp
        elif zi.compress_type == ZIP_DEFLATED:
            out = _inflate(comp, zi.file_size)
        else:
            raise NotImplementedError("compression type %d is not supported" % zi.compress_type)
        if len(out) != zi.file_size:
            raise BadZipFile("File size of " + repr(zi.filename) + " differs from expected")
        got = _zlib.crc32(out) & _U32MAX
        if got != zi.CRC:
            raise BadZipFile("Bad CRC-32 for file " + repr(zi.filename))
        return out

    def testzip(self) -> object:
        for zi in self._entries:
            try:
                self.read(zi)
            except BadZipFile:
                return zi.filename
        return None

    def _check_open_write(self) -> None:
        if self.closed:
            raise ValueError("Attempt to write to ZIP archive that was already closed")
        if self._mode == "r":
            raise ValueError("write() on read-only ZipFile")

    def _store(self, zi: ZipInfo, data: bytes, compress_type: int, compresslevel: int) -> None:
        if compress_type == ZIP_STORED:
            payload = data
        elif compress_type == ZIP_DEFLATED:
            payload = _deflate(data, compresslevel)
        elif compress_type == ZIP_BZIP2 or compress_type == ZIP_LZMA:
            raise NotImplementedError("compression type %d is not supported" % compress_type)
        else:
            raise NotImplementedError("compression type %d is not supported" % compress_type)
        zi.compress_type = compress_type
        zi.file_size = len_bytes(data)
        zi.CRC = _cz.crc32(0, data, len_bytes(data)) & _U32MAX
        zi.compress_size = len_bytes(payload)
        zi.flag_bits = zi.flag_bits & ~0x8
        nb = zi.filename.encode("utf-8")
        if len_bytes(nb) != len(zi.filename):
            zi.flag_bits = zi.flag_bits | 0x800
        if zi.is_dir():
            zi.external_attr = zi.external_attr | 0x10
        self._entries.append(zi)
        self._payloads.append(payload)

    def write(self, filename: str, arcname: object = None,
              compress_type: int = -1, compresslevel: int = -1) -> None:
        # compress_type 用 -1 哨兵而不是 None：ZIP_STORED==0，None/0 同形
        # 会把显式 ZIP_STORED 吞掉（0.324 边界）。compresslevel -1 即 zlib
        # 默认级别，与 CPython None 的内部口径一致。
        self._check_open_write()
        zi = ZipInfo.from_file(filename, arcname)
        ct = self._compression
        if compress_type >= 0:
            ct = compress_type
        cl = self._compresslevel
        if compresslevel >= -1:
            cl = compresslevel
        if zi.is_dir():
            self._store(zi, b"", ZIP_STORED, cl)
            return
        data = _gzipio.read_bin(filename)
        self._store(zi, data, ct, cl)

    def writestr(self, zinfo_or_arcname: object, data: object,
                 compress_type: int = -1, compresslevel: int = -1) -> None:
        # -1 哨兵口径同 write()（ZIP_STORED==0 与 None 同形）。
        self._check_open_write()
        zi: ZipInfo = cast(ZipInfo, zinfo_or_arcname)
        if not isinstance(zinfo_or_arcname, ZipInfo):
            zi = ZipInfo(cast(str, zinfo_or_arcname))
            zi.external_attr = 0o600 << 16
        ct = self._compression
        if compress_type >= 0:
            ct = compress_type
        elif isinstance(zinfo_or_arcname, ZipInfo):
            ct = zi.compress_type
        cl = self._compresslevel
        if compresslevel >= -1:
            cl = compresslevel
        b: bytes = data
        self._store(zi, b, ct, cl)

    def _extract_member(self, member: object, targetpath: str, pwd: object) -> str:
        zi = self._resolve(member)
        parts: list[str] = []
        for p in zi.filename.split("/"):
            if p == "" or p == "." or p == "..":
                continue
            parts.append(p)
        arcname = "/".join(parts)
        target = _osp.normpath(_osp.join(targetpath, arcname))
        if zi.is_dir():
            os.makedirs(target, exist_ok=True)
            return target
        upper = _osp.dirname(target)
        if not _osp.exists(upper):
            os.makedirs(upper, exist_ok=True)
        data = self.read(zi, pwd)
        _gzipio.write_bin(target, data)
        return target

    def extract(self, member: object, path: object = None, pwd: object = None) -> str:
        if self.closed:
            raise ValueError("extract() on closed ZipFile")
        tp = "."
        if not (path is None):
            tp = path
        return self._extract_member(member, tp, pwd)

    def extractall(self, path: object = None, members: object = None, pwd: object = None) -> None:
        if self.closed:
            raise ValueError("extractall() on closed ZipFile")
        tp = "."
        if not (path is None):
            tp = path
        if members is None:
            for zi in self._entries:
                self._extract_member(zi, tp, pwd)
        else:
            ms: list[object] = list(members)
            for m in ms:
                self._extract_member(m, tp, pwd)

    def close(self) -> None:
        if self.closed:
            return
        self.closed = True
        if self._mode == "r":
            return
        n = len(self._entries)
        if n > 0xFFFF:
            raise LargeZipFile("Zip64 is not supported: too many entries")
        parts: list[bytes] = []
        offsets: list[int] = []
        pos = 0
        i = 0
        while i < n:
            zi = self._entries[i]
            if zi.file_size > _U32MAX or zi.compress_size > _U32MAX:
                raise LargeZipFile("Zip64 is not supported: entry too large")
            offsets.append(pos)
            nb = zi.filename.encode("utf-8")
            t, d = _dos_time(zi.date_time)
            ev = zi.extract_version
            if ev < 20:
                ev = 20
            lh = b"PK\x03\x04" + _p16(ev) + _p16(zi.flag_bits) + _p16(zi.compress_type)
            lh = lh + _p16(t) + _p16(d) + _p32(zi.CRC) + _p32(zi.compress_size) + _p32(zi.file_size)
            lh = lh + _p16(len_bytes(nb)) + _p16(len_bytes(zi.extra)) + nb + zi.extra
            parts.append(lh)
            parts.append(self._payloads[i])
            pos += len_bytes(lh) + len_bytes(self._payloads[i])
            i += 1
        cd_start = pos
        cd_size = 0
        i = 0
        while i < n:
            zi = self._entries[i]
            nb = zi.filename.encode("utf-8")
            t, d = _dos_time(zi.date_time)
            ev = zi.extract_version
            if ev < 20:
                ev = 20
            cv = zi.create_version
            if cv < 20:
                cv = 20
            ch = b"PK\x01\x02" + _p16(cv | (zi.create_system << 8)) + _p16(ev)
            ch = ch + _p16(zi.flag_bits) + _p16(zi.compress_type) + _p16(t) + _p16(d)
            ch = ch + _p32(zi.CRC) + _p32(zi.compress_size) + _p32(zi.file_size)
            ch = ch + _p16(len_bytes(nb)) + _p16(len_bytes(zi.extra)) + _p16(len_bytes(zi.comment))
            ch = ch + _p16(zi.volume) + _p16(zi.internal_attr) + _p32(zi.external_attr)
            ch = ch + _p32(offsets[i]) + nb + zi.extra + zi.comment
            parts.append(ch)
            cd_size += len_bytes(ch)
            i += 1
        if cd_start > _U32MAX or cd_size > _U32MAX:
            raise LargeZipFile("Zip64 is not supported: archive too large")
        eocd = b"PK\x05\x06" + _p16(0) + _p16(0) + _p16(n) + _p16(n)
        eocd = eocd + _p32(cd_size) + _p32(cd_start) + _p16(len_bytes(self.comment)) + self.comment
        parts.append(eocd)
        out = b""
        for p in parts:
            out = out + p
        _gzipio.write_bin(self.filename, out)

    def open(self, name: object, mode: str = "r", pwd: object = None,
             *, force_zip64: bool = False) -> "_ZipExtFile":
        if self.closed:
            raise ValueError("open() on closed ZipFile")
        if mode == "r":
            zi = self._resolve(name)
            return _ZipExtFile(self, zi, False)
        if mode == "w":
            self._check_open_write()
            s: str = name
            zi = ZipInfo(s)
            zi.compress_type = self._compression
            return _ZipExtFile(self, zi, True)
        raise ValueError("open() requires mode 'r' or 'w'")

    def __enter__(self) -> "ZipFile":
        return self

    def __exit__(self, exc_type: object, exc_val: object, exc_tb: object) -> bool:
        self.close()
        return False


class _ZipExtFile:
    """ZipFile.open 的条目流（整读整写口径：r 一次解包，w 在 close 落盘）。"""
    name: str
    _zf: ZipFile
    _zi: ZipInfo
    _wmode: bool
    _buf: bytes
    _pos: int
    _closed: bool
    _wparts: list[bytes]

    def __init__(self, zf: ZipFile, zi: ZipInfo, wmode: bool) -> None:
        self._zf = zf
        self._zi = zi
        self._wmode = wmode
        self.name = zi.filename
        self._buf = b""
        self._pos = 0
        self._closed = False
        self._wparts = []
        if not wmode:
            self._buf = zf.read(zi)

    def readable(self) -> bool:
        return not self._wmode

    def writable(self) -> bool:
        return self._wmode

    def read(self, size: int = -1) -> bytes:
        if self._closed:
            raise ValueError("I/O operation on closed file")
        if self._wmode:
            raise OSError("read() on write-only ZipExtFile")
        n = len(self._buf)
        end = n
        if size >= 0 and self._pos + size < n:
            end = self._pos + size
        chunk = self._buf[self._pos:end]
        self._pos = end
        return chunk

    def readline(self, size: int = -1) -> bytes:
        if self._closed:
            raise ValueError("I/O operation on closed file")
        if self._wmode:
            raise OSError("readline() on write-only ZipExtFile")
        n = len(self._buf)
        if self._pos >= n:
            return b""
        nl = self._buf.find(b"\n", self._pos)
        end = n
        if nl >= 0:
            end = nl + 1
        if size >= 0 and self._pos + size < end:
            end = self._pos + size
        chunk = self._buf[self._pos:end]
        self._pos = end
        return chunk

    def readlines(self) -> list[bytes]:
        out: list[bytes] = []
        while True:
            line = self.readline()
            if len(line) == 0:
                break
            out.append(line)
        return out

    def seek(self, off: int, whence: int = 0) -> int:
        if self._closed:
            raise ValueError("I/O operation on closed file")
        if self._wmode:
            raise OSError("seek() on write-only ZipExtFile")
        n = len(self._buf)
        target = off
        if whence == 1:
            target = self._pos + off
        elif whence == 2:
            target = n + off
        if target < 0:
            target = 0
        if target > n:
            target = n
        self._pos = target
        return self._pos

    def tell(self) -> int:
        if self._wmode:
            t = 0
            for p in self._wparts:
                t += len(p)
            return t
        return self._pos

    def write(self, data: object) -> int:
        if self._closed:
            raise ValueError("I/O operation on closed file")
        if not self._wmode:
            raise OSError("write() on read-only ZipExtFile")
        b: bytes = data
        self._wparts.append(b)
        return len(b)

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        if self._wmode:
            payload = b""
            for p in self._wparts:
                payload = payload + p
            self._zf.writestr(self._zi, payload)

    def __iter__(self) -> list[bytes]:
        return self.readlines()

    def __enter__(self) -> "_ZipExtFile":
        return self

    def __exit__(self, exc_type: object, exc_val: object, exc_tb: object) -> bool:
        self.close()
        return False


def is_zipfile(filename: str) -> bool:
    try:
        data = _gzipio.read_bin(filename)
    except OSError:
        return False
    try:
        _find_eocd(data)
        return True
    except BadZipFile:
        return False
