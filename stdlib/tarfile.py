"""tarfile 模块的 pyadamas 子集（纯 pyadamas 源码，无新增 C 胶水）。

读写 tar 归档：ustar（POSIX.1-1988）头解析/生成，512 字节块，八进制
校验和；读写经 fileobj（BytesIO/真 file）或路径，gzip 透明（r:gz/w:gz
经 stdlib/gzip）。纯源码实现：数值字段八进制整数算法、校验和逐字节求和、
长名 prefix 拆分；只读文件的字节面走 _gzipio，stat/readlink/symlink 复用
既有 stdlib/_shutilc 胶水（不新增 C）。

覆盖：TarInfo（字段 + isfile/isdir/issym/islnk/isdev 等 + frombuf/tobuf）、
TarFile（r/w/a/x、getmembers/getnames/getmember、extractfile、add/addfile/
gettarinfo、extract/extractall、close、上下文管理、迭代）、open/is_tarfile、
USTAR_FORMAT/GNU_FORMAT/PAX_FORMAT 常量、TarError/ReadError/CompressionError/
ExtractError/HeaderError 异常族。

语义边界（轻量标尺）：
- **整读整写**（同 zipfile/GzipFile 口径）：读一次性把归档载入内存，写把
  全部块攒在内存、close 时一次落盘（两块全零尾 + 补齐到 RECORDSIZE，与
  CPython 字节级一致）。内存与归档大小成正比；超大归档请走
  `from python import tarfile` 桥接。
- **只写 ustar**：DEFAULT_FORMAT 取 PAX_FORMAT（与 CPython 同值），但写入
  永远产出 ustar 头（GNU/PAX 扩展头 L/K/x/g **不写**）。短 ASCII 名成员
  与 CPython 的默认写出**逐字节一致**；长名按 prefix(155)+name(100) 在
  '/' 处拆分（同 CPython USTAR_FORMAT），拆不开抛 ValueError("name is too
  long")，linkname 超 100 字节同样抛 ValueError。读侧支持 ustar + GNU 长名
  长链接（L/K，可连续叠加）+ pax x/g（path/linkpath/size/mtime/uid/gid/
  uname/gname）；稀疏文件（S）与 Solaris X 头不解析（按未知类型处理）。
- **压缩只做 gzip**：r/w/x/a 之外的 ":gz" 后缀（stdlib/gzip；写出 mtime=0
  可复现，压缩字节不追求与 CPython 一致，但解压出的载荷逐字节相同）；
  bz2/xz/zst 抛 CompressionError；r:*/r 按魔数嗅探。a:gz 不支持。
- **数值字段是 int64**：写侧八进制 ustar 字段（8^(digits-1) 上限，超了抛
  ValueError("overflow in number field")，CPython 在 GNU 格式下会转
  base-256）；读侧接受 base-256（0o200/0o377 前导），但字段超过 63 位抛
  InvalidHeaderError（CPython 任意精度）。pax 的 mtime 按 int 收（CPython
  是 float）。
- **元数据取自 lstat 的可见面**：_shutilc.stat 只给 mode/mtime/size/kind，
  uid/gid 一律 0、uname/gname 空（CPython 填真实属主）；add() 不做硬链接
  合并（没有 inode 可比），LNKTYPE 只在手写 TarInfo 时出现。
- **解压过滤不做**：CPython 3.12+ 默认 data_filter 不做，filter=/
  numeric_owner=/set_attrs 之外的过滤参数不存在（extract/extractall 都落
  盘 + chmod + utime）。名字先净化（剥前导 '/'、丢 ''/'.'/'..' 段，同
  zipfile），所以 `../x` 这种成员落到解压根下而不是抛 OutsideDestinationError
  （CPython 默认 filter 抛 FilterError）。uid/gid 不落盘（无 chown）。
- **流式模式**：r|/w| 按普通 r/w 处理（整读整写），不模拟 _Stream 分块语义。
- **错误时机前移**：坏归档在 open（而非首次 getmembers）抛 ReadError；
  r/r:* 把首个头错误包成 ReadError("file could not be opened successfully:
  …")，r:/显式压缩模式保留原始 InvalidHeaderError（CPython 同口径）。
  关闭后的操作抛 OSError("TarFile is closed")、模式不符抛
  OSError("bad operation for mode 'w'")（同 CPython 3.14）。
- **错误类型对齐、消息不逐字**：ReadError/CompressionError/ExtractError/
  HeaderError 等类型与 CPython 同名同继承；消息是合理文本。is_tarfile 对
  不存在的路径返回 False（CPython 3.14 会漏出 FileNotFoundError）。
- **静态类型边界**（pa 语言面）：extractfile 的静态返回是 BytesIO，目录/
  设备成员返回空 BytesIO（CPython 返回 None）；gettarinfo 对 fifo/设备/
  socket 抛 ValueError（CPython 返回 None 让 add 跳过）。imported 类经模块
  限定名的类方法调用（`tarfile.TarInfo.frombuf`）不受支持，请
  `from tarfile import TarInfo` 后用 `TarInfo.frombuf`；同理形参别标成
  导入类（`f: TarFile`），用局部变量或 object + cast。
"""

import os
import os.path as _osp
import _gzipio
import gzip as _gzip
from io import BytesIO
cimport _shutilc as _sc
from typing import cast


# ── 常量（与 CPython 3.14 同值；type 常量是 bytes）─────────────────
NUL: bytes = b"\x00"
BLOCKSIZE: int = 512
RECORDSIZE: int = BLOCKSIZE * 20

GNU_MAGIC: bytes = b"ustar  \x00"
POSIX_MAGIC: bytes = b"ustar\x00" + b"00"

LENGTH_NAME: int = 100
LENGTH_LINK: int = 100
LENGTH_PREFIX: int = 155

REGTYPE: bytes = b"0"
AREGTYPE: bytes = b"\x00"
LNKTYPE: bytes = b"1"
SYMTYPE: bytes = b"2"
CHRTYPE: bytes = b"3"
BLKTYPE: bytes = b"4"
DIRTYPE: bytes = b"5"
FIFOTYPE: bytes = b"6"
CONTTYPE: bytes = b"7"

GNUTYPE_LONGNAME: bytes = b"L"
GNUTYPE_LONGLINK: bytes = b"K"
GNUTYPE_SPARSE: bytes = b"S"

XHDTYPE: bytes = b"x"
XGLTYPE: bytes = b"g"

USTAR_FORMAT: int = 0
GNU_FORMAT: int = 1
PAX_FORMAT: int = 2
# 与 CPython 同值（PAX）：短 ASCII 名成员的默认写入与 CPython 字节级一致；
# 但本实现只有 ustar 编码一种（不写 pax 扩展头），见模块文档。
DEFAULT_FORMAT: int = PAX_FORMAT

ENCODING: str = "utf-8"

# 视为「普通文件数据」的类型。
REGULAR_TYPES: tuple[bytes, bytes, bytes, bytes] = (REGTYPE, AREGTYPE, CONTTYPE, GNUTYPE_SPARSE)
SUPPORTED_TYPES: tuple[bytes, bytes, bytes, bytes, bytes, bytes, bytes, bytes, bytes, bytes, bytes, bytes] = (
    REGTYPE, AREGTYPE, LNKTYPE, SYMTYPE, DIRTYPE, FIFOTYPE,
    CONTTYPE, CHRTYPE, BLKTYPE, GNUTYPE_LONGNAME, GNUTYPE_LONGLINK, GNUTYPE_SPARSE,
)


# pa 的 bytes-in-tuple 对「切片产出的 bytes」判不相等（native 快路径按
# 指针/长度比；docs 记的既有运行时问题），所以内部一律用显式 == 比较，
# 不用 in。下面的谓词就是为此存在的。
def _is_regular_type(t: bytes) -> bool:
    return t == REGTYPE or t == AREGTYPE or t == CONTTYPE or t == GNUTYPE_SPARSE


def _is_supported_type(t: bytes) -> bool:
    return (t == REGTYPE or t == AREGTYPE or t == LNKTYPE or t == SYMTYPE
            or t == DIRTYPE or t == FIFOTYPE or t == CONTTYPE or t == CHRTYPE
            or t == BLKTYPE or t == GNUTYPE_LONGNAME or t == GNUTYPE_LONGLINK
            or t == GNUTYPE_SPARSE)

# 数值字段（名字, 宽度）；与 CPython USTAR_FORMAT 的字段偏移一致。
_FIELD_NAMES: tuple[str, str, str, str, str, str, str, str, str, str, str, str, str, str, str] = (
    "name", "mode", "uid", "gid", "size", "mtime", "chksum", "typeflag",
    "linkname", "magic", "version", "uname", "gname", "devmajor", "devminor",
)


# ── 异常族 ────────────────────────────────────────────────────────
class TarError(Exception):
    pass


class ReadError(TarError):
    pass


class CompressionError(TarError):
    pass


class StreamError(TarError):
    pass


class ExtractError(TarError):
    pass


class HeaderError(TarError):
    pass


class InvalidHeaderError(HeaderError):
    pass


class EmptyHeaderError(HeaderError):
    pass


class TruncatedHeaderError(HeaderError):
    pass


class EOFHeaderError(HeaderError):
    pass


# ── 字段编解码 ────────────────────────────────────────────────────
def _stn(s: str, length: int) -> bytes:
    """str -> 定长 NUL 填充字节串（超长按字节截断）。"""
    enc: bytes = s.encode(ENCODING)
    n: int = len_bytes(enc)
    if n > length:
        return enc[:length]
    if n < length:
        return enc + NUL * (length - n)
    return enc


def _nts(b: bytes) -> str:
    """定长 NUL 终止字节串 -> str（非法 UTF-8 按 replace）。"""
    p: int = b.find(NUL)
    if p >= 0:
        b = b[:p]
    return b.decode(ENCODING, "replace")


def _nti(b: bytes) -> int:
    """数值字段 -> int：八进制 ASCII，或 0o200/0o377 前导的 base-256。"""
    n: int = len_bytes(b)
    if n == 0:
        return 0
    c: int = b[0]
    if c == 0o200 or c == 0o377:
        digits: int = n - 1
        if digits > 7:
            raise InvalidHeaderError("base-256 number field out of int64 range")
        v: int = 0
        i: int = 1
        while i < n:
            v = (v << 8) + b[i]
            i += 1
        if c == 0o377:
            v = v - (1 << (8 * digits))
        return v
    s: str = _nts(b).strip()
    if len(s) == 0:
        return 0
    try:
        return int(s, 8)
    except ValueError:
        raise InvalidHeaderError("invalid header")


def _oct(v: int, width: int) -> str:
    """非负 int -> width 位八进制串（pa 的 % 不支持动态宽度）。"""
    s: str = ""
    x: int = v
    while x > 0:
        s = str(x % 8) + s
        x = x // 8
    if len(s) == 0:
        s = "0"
    while len(s) < width:
        s = "0" + s
    return s


def _itn(v: int, digits: int) -> bytes:
    """int -> 八进制数值字段（digits 含结尾 NUL）。溢出抛 ValueError。"""
    limit: int = 8 ** (digits - 1)
    if v < 0 or v >= limit:
        raise ValueError("overflow in number field")
    return _oct(v, digits - 1).encode("ascii") + NUL


def _stn_magic() -> bytes:
    return POSIX_MAGIC


def _calc_chksums(buf: bytes) -> tuple[int, int]:
    """(无符号校验和, 有符号校验和)：跳过 148..156 的 chksum 字段。"""
    unsigned: int = 0
    signed: int = 0
    i: int = 0
    n: int = len_bytes(buf)
    while i < n:
        if i < 148 or i > 155:
            c: int = buf[i]
            unsigned += c
            if c > 127:
                signed += c - 256
            else:
                signed += c
        i += 1
    # chksum 字段按 8 个空格计入。
    unsigned += 8 * 32
    signed += 8 * 32
    return (unsigned, signed)


def _roundup(n: int, bs: int) -> int:
    blocks: int = n // bs
    if n % bs > 0:
        blocks += 1
    return blocks * bs


def _posix_split_name(name: str) -> tuple[str, str]:
    """长名按 '/' 拆成 (prefix <= 155, name <= 100)；拆不开抛 ValueError。"""
    components: list[str] = name.split("/")
    i: int = 1
    prefix: str = ""
    rest: str = name
    while i < len(components):
        prefix = "/".join(components[:i])
        rest = "/".join(components[i:])
        if len_bytes(prefix.encode(ENCODING)) <= LENGTH_PREFIX and \
                len_bytes(rest.encode(ENCODING)) <= LENGTH_NAME:
            return (prefix, rest)
        i += 1
    raise ValueError("name is too long")


class TarInfo:
    """tar 成员元数据（CPython TarInfo 的常用面）。"""

    name: str = ""
    mode: int = 0o644
    uid: int = 0
    gid: int = 0
    size: int = 0
    mtime: int = 0
    chksum: int = 0
    type: bytes = REGTYPE
    linkname: str = ""
    uname: str = ""
    gname: str = ""
    devmajor: int = 0
    devminor: int = 0
    offset: int = 0
    offset_data: int = 0

    def __init__(self, name: str = "") -> None:
        self.name = name
        self.mode = 0o644
        self.uid = 0
        self.gid = 0
        self.size = 0
        self.mtime = 0
        self.chksum = 0
        self.type = REGTYPE
        self.linkname = ""
        self.uname = ""
        self.gname = ""
        self.devmajor = 0
        self.devminor = 0
        self.offset = 0
        self.offset_data = 0

    # ── 类型谓词 ────────────────────────────────────────────────
    def isfile(self) -> bool:
        return _is_regular_type(self.type)

    def isreg(self) -> bool:
        return _is_regular_type(self.type)

    def islnk(self) -> bool:
        return self.type == LNKTYPE

    def issym(self) -> bool:
        return self.type == SYMTYPE

    def isdir(self) -> bool:
        return self.type == DIRTYPE

    def isdev(self) -> bool:
        return self.type == CHRTYPE or self.type == BLKTYPE or self.type == FIFOTYPE

    def ischr(self) -> bool:
        return self.type == CHRTYPE

    def isblk(self) -> bool:
        return self.type == BLKTYPE

    def isfifo(self) -> bool:
        return self.type == FIFOTYPE

    def iscont(self) -> bool:
        return self.type == CONTTYPE

    def issparse(self) -> bool:
        return self.type == GNUTYPE_SPARSE

    # ── 头解析/生成 ─────────────────────────────────────────────
    @classmethod
    def frombuf(cls, buf: bytes, encoding: str = "utf-8", errors: str = "replace") -> "TarInfo":
        """512 字节头 -> TarInfo（校验和不符抛 InvalidHeaderError）。"""
        n: int = len_bytes(buf)
        if n == 0:
            raise EmptyHeaderError("empty header")
        if n != BLOCKSIZE:
            raise TruncatedHeaderError("truncated header")
        if buf == NUL * BLOCKSIZE:
            raise EOFHeaderError("end of file header")

        chksum: int = _nti(buf[148:156])
        sums: tuple[int, int] = _calc_chksums(buf)
        if chksum != sums[0] and chksum != sums[1]:
            raise InvalidHeaderError("bad checksum")

        obj = cls()
        obj.name = _nts(buf[0:100])
        obj.mode = _nti(buf[100:108])
        obj.uid = _nti(buf[108:116])
        obj.gid = _nti(buf[116:124])
        obj.size = _nti(buf[124:136])
        obj.mtime = _nti(buf[136:148])
        obj.chksum = chksum
        obj.type = buf[156:157]
        obj.linkname = _nts(buf[157:257])
        obj.uname = _nts(buf[265:297])
        obj.gname = _nts(buf[297:329])
        obj.devmajor = _nti(buf[329:337])
        obj.devminor = _nti(buf[337:345])
        prefix: str = _nts(buf[345:500])

        # 老 V7 格式用「名字带尾斜杠的普通文件头」表示目录。
        if obj.type == AREGTYPE and obj.name.endswith("/"):
            obj.type = DIRTYPE
        if len_bytes(prefix) > 0:
            obj.name = prefix + "/" + obj.name
        if obj.isdir():
            obj.name = obj.name.rstrip("/")
        return obj

    def tobuf(self, format: int = 0, encoding: str = "utf-8", errors: str = "replace") -> bytes:
        """TarInfo -> 512 字节 ustar 头（只写 ustar；format 参数收下不区分）。"""
        name: str = self.name
        # CPython get_info()：目录名在头里补尾斜杠（读回时又剥掉）。
        if self.type == DIRTYPE and not name.endswith("/"):
            name = name + "/"
        prefix: str = ""
        if len_bytes(name.encode(ENCODING)) > LENGTH_NAME:
            prefix, name = _posix_split_name(name)
        if len_bytes(self.linkname.encode(ENCODING)) > LENGTH_LINK:
            raise ValueError("linkname is too long")

        filetype: bytes = self.type
        devmajor: bytes = NUL * 8
        devminor: bytes = NUL * 8
        if filetype == CHRTYPE or filetype == BLKTYPE:
            devmajor = _itn(self.devmajor, 8)
            devminor = _itn(self.devminor, 8)

        parts: list[bytes] = []
        parts.append(_stn(name, 100))
        parts.append(_itn(self.mode & 0o7777, 8))
        parts.append(_itn(self.uid, 8))
        parts.append(_itn(self.gid, 8))
        parts.append(_itn(self.size, 12))
        parts.append(_itn(self.mtime, 12))
        parts.append(b"        ")  # chksum 占位（8 空格）
        parts.append(filetype[:1] if len_bytes(filetype) > 0 else REGTYPE)
        parts.append(_stn(self.linkname, 100))
        parts.append(POSIX_MAGIC)
        parts.append(_stn(self.uname, 32))
        parts.append(_stn(self.gname, 32))
        parts.append(devmajor)
        parts.append(devminor)
        parts.append(_stn(prefix, 155))
        body: bytes = b""
        for p in parts:
            body = body + p
        body = body + NUL * (BLOCKSIZE - len_bytes(body))
        chksum: int = _calc_chksums(body)[0]
        ck: bytes = _oct(chksum, 6).encode("ascii") + NUL
        return body[:148] + ck + body[155:]

    def get_info(self) -> dict[str, object]:
        """属性字典（CPython get_info；目录名补尾斜杠）。"""
        info: dict[str, object] = {}
        info["name"] = self.name
        info["mode"] = self.mode & 0o7777
        info["uid"] = self.uid
        info["gid"] = self.gid
        info["size"] = self.size
        info["mtime"] = self.mtime
        info["chksum"] = self.chksum
        info["type"] = self.type
        info["linkname"] = self.linkname
        info["uname"] = self.uname
        info["gname"] = self.gname
        info["devmajor"] = self.devmajor
        info["devminor"] = self.devminor
        if self.type == DIRTYPE and not self.name.endswith("/"):
            info["name"] = self.name + "/"
        return info

    def __repr__(self) -> str:
        return "<TarInfo " + repr(self.name) + ">"

class TarFile:
    """tar 归档句柄（整读整写口径，见模块文档）。"""

    name: str = ""
    mode: str = "r"
    format: int = PAX_FORMAT
    encoding: str = "utf-8"
    errors: str = "replace"
    debuglevel: int = 0
    dereference: bool = False
    ignore_zeros: bool = False
    errorlevel: int = 1
    closed: bool = False

    members: list[TarInfo]
    fileobj: file
    offset: int = 0

    _comp: str
    _data: bytes
    _wparts: list[bytes]
    _compresslevel: int
    _auto: bool = False
    _end: int = 0
    _pax_global: dict[str, str]

    def __init__(self, name: str = "", mode: str = "r", fileobj: file = None,
                 format: int = PAX_FORMAT, encoding: str = "utf-8",
                 errors: str = "replace", compresslevel: int = 9) -> None:
        self.name = name
        self.format = format
        self.encoding = encoding
        self.errors = errors
        self.debuglevel = 0
        self.dereference = False
        self.ignore_zeros = False
        self.errorlevel = 1
        self.closed = False
        self.members = []
        self.fileobj = fileobj
        self.offset = 0
        self._data = b""
        self._wparts = []
        self._pax_global = {}
        self._compresslevel = compresslevel

        filemode: str = mode
        comp: str = ""
        if ":" in mode:
            parts: list[str] = mode.split(":", 1)
            filemode = parts[0]
            comp = parts[1]
            if len(filemode) == 0:
                filemode = "r"
            if len(comp) == 0:
                comp = "tar"
        elif "|" in mode:
            # 流式模式按整读整写处理（文档化边界）。
            parts = mode.split("|", 1)
            filemode = parts[0]
            if len(filemode) == 0:
                filemode = "r"
            comp = "tar"
        if len(comp) == 0:
            comp = "tar"
        self._comp = comp
        self.mode = filemode

        if filemode != "r" and filemode != "w" and filemode != "x" and filemode != "a":
            raise ValueError("mode must be 'r', 'a', 'w' or 'x'")
        if filemode != "r" and (comp == "gz" or comp == "*"):
            pass
        if filemode == "a" and (comp == "gz" or comp == "bz2"):
            raise CompressionError("'a' mode is not allowed for compressed archives")

        # "r"/"r:*" 走 CPython 的自动嗅探口径：首个头坏掉包成 ReadError。
        self._auto = mode == "r" or mode == "r:*"
        if filemode == "r":
            self._read_all(comp)
            self._parse()
        elif filemode == "a":
            raw: bytes = self._read_raw()
            self._data = raw
            if len_bytes(raw) > 0:
                self._parse()
                self._wparts.append(raw[0:self._end])
                self.offset = self._end
        elif filemode == "x":
            if len(name) > 0 and _osp.exists(name):
                raise FileExistsError("File exists: " + repr(name))

    # ── 读：取字节、解压、解析 ───────────────────────────────────
    def _read_raw(self) -> bytes:
        if self.fileobj is not None:
            return cast(bytes, self.fileobj.read())
        if len(self.name) > 0:
            if not _osp.exists(self.name):
                return b""
            return _gzipio.read_bin(self.name)
        raise ValueError("nothing to open")

    def _read_all(self, comp: str) -> None:
        like: bytes = self._read_raw()
        if len_bytes(like) == 0 and len(self.name) > 0 and self.fileobj is None:
            raise FileNotFoundError("No such file: " + repr(self.name))
        self._data = self._decompress(like, comp)

    def _decompress(self, raw: bytes, comp: str) -> bytes:
        if comp == "*":
            if len_bytes(raw) >= 2 and raw[0] == 0x1F and raw[1] == 0x8B:
                comp = "gz"
            else:
                comp = "tar"
        if comp == "gz":
            try:
                return _gzip.decompress(raw)
            except OSError as e:
                raise ReadError("not a gzip file")
            except EOFError as e:
                raise ReadError("not a gzip file")
        if comp == "tar" or len(comp) == 0:
            return raw
        raise CompressionError("unknown compression type " + repr(comp))

    def _parse(self) -> None:
        data: bytes = self._data
        total: int = len_bytes(data)
        pos: int = 0
        first: bool = True
        self._end = 0
        while pos + BLOCKSIZE <= total:
            buf: bytes = data[pos:pos + BLOCKSIZE]
            if buf == NUL * BLOCKSIZE:
                self._end = pos
                return
            ti: TarInfo = self._header_at(data, pos, first)
            first = False
            ti.offset = pos
            pos += BLOCKSIZE
            if len(self._pax_global) > 0:
                _apply_pax(ti, self._pax_global)
            ti = self._proc_header(ti, pos)
            # _proc_header 返回的是「真成员」头，数据区相对它自己的头。
            hdr_end: int = ti.offset + BLOCKSIZE
            ti.offset_data = hdr_end
            if ti.isreg() or not _is_supported_type(ti.type):
                end: int = hdr_end + _roundup(ti.size, BLOCKSIZE)
                if end > total:
                    raise ReadError("unexpected end of data")
                pos = end
            else:
                pos = hdr_end
            self.members.append(ti)
        self._end = pos
        if pos < total:
            # 尾部残留不足一个块：只有全零才算正常结束。
            rest: bytes = data[pos:total]
            k: int = 0
            while k < len_bytes(rest) and rest[k] == 0:
                k += 1
            if k < len_bytes(rest):
                raise ReadError("unexpected end of data")

    def _header_at(self, data: bytes, pos: int, first: bool) -> TarInfo:
        """读一个头块；自动嗅探模式下首个头坏掉包成 ReadError（CPython 口径）。"""
        buf: bytes = data[pos:pos + BLOCKSIZE]
        if first:
            try:
                return TarInfo.frombuf(buf, self.encoding, self.errors)
            except HeaderError as e:
                if self._auto:
                    raise ReadError("file could not be opened successfully: " + str(e))
                raise
        return TarInfo.frombuf(buf, self.encoding, self.errors)

    def _next_header(self, pos: int) -> TarInfo:
        data: bytes = self._data
        total: int = len_bytes(data)
        if pos + BLOCKSIZE > total:
            raise ReadError("unexpected end of data")
        buf: bytes = data[pos:pos + BLOCKSIZE]
        if buf == NUL * BLOCKSIZE:
            raise ReadError("unexpected end of data")
        nxt: TarInfo = TarInfo.frombuf(buf, self.encoding, self.errors)
        nxt.offset = pos
        return nxt

    def _proc_header(self, ti: TarInfo, data_pos: int) -> TarInfo:
        """吃掉 GNU 长名/长链接与 pax 扩展头，返回真成员头。

        GNU 会连续写 K（长 linkname）再 L（长 name）再真头，pax 的 x 头同理
        可叠加，所以这里用循环累积待定字段而不是递归逐层覆盖。"""
        data: bytes = self._data
        total: int = len_bytes(data)
        cur: TarInfo = ti
        cur_pos: int = data_pos
        pend_name: str = ""
        pend_link: str = ""
        pend_pax: dict[str, str] = {}
        while True:
            ty: bytes = cur.type
            if ty == GNUTYPE_LONGNAME or ty == GNUTYPE_LONGLINK:
                if cur_pos + cur.size > total:
                    raise ReadError("unexpected end of data")
                g_payload: bytes = data[cur_pos:cur_pos + cur.size]
                g_pos: int = cur_pos + _roundup(cur.size, BLOCKSIZE)
                g_nxt: TarInfo = self._next_header(g_pos)
                if ty == GNUTYPE_LONGNAME:
                    pend_name = _nts(g_payload)
                else:
                    pend_link = _nts(g_payload)
                cur = g_nxt
                cur_pos = g_pos + BLOCKSIZE
                continue
            if ty == XHDTYPE or ty == XGLTYPE:
                if cur_pos + cur.size > total:
                    raise ReadError("unexpected end of data")
                x_payload: bytes = data[cur_pos:cur_pos + cur.size]
                x_pos: int = cur_pos + _roundup(cur.size, BLOCKSIZE)
                hdrs: dict[str, str] = _parse_pax(x_payload)
                if ty == XGLTYPE:
                    for k in hdrs:
                        self._pax_global[k] = hdrs[k]
                else:
                    for k in hdrs:
                        pend_pax[k] = hdrs[k]
                cur = self._next_header(x_pos)
                cur_pos = x_pos + BLOCKSIZE
                continue
            return _finish_header(cur, pend_name, pend_link, pend_pax)

    # ── 公共读接口 ──────────────────────────────────────────────
    def _check(self, allow: str) -> None:
        # CPython 3.14：关闭后与模式不符都是 OSError（不是 ValueError）。
        if self.closed:
            raise OSError("TarFile is closed")
        if len(allow) > 0 and not (self.mode in allow):
            raise OSError("bad operation for mode " + repr(self.mode))

    def _find_index(self, nm: str) -> int:
        i: int = len(self.members) - 1
        while i >= 0:
            if self.members[i].name == nm:
                return i
            i -= 1
        return -1

    def getmembers(self) -> list[TarInfo]:
        self._check("")
        return self.members

    def getnames(self) -> list[str]:
        out: list[str] = []
        for m in self.getmembers():
            out.append(m.name)
        return out

    def getmember(self, name: str) -> TarInfo:
        i: int = self._find_index(name.rstrip("/"))
        if i < 0:
            raise KeyError("filename " + repr(name) + " not found")
        return self.members[i]

    def extractfile(self, member: object) -> BytesIO:
        """普通文件返回 BytesIO 视图；目录/设备等非数据成员返回空 BytesIO
        （CPython 返回 None，见模块文档的边界）。"""
        self._check("r")
        if isinstance(member, TarInfo):
            ti: TarInfo = cast(TarInfo, member)
        else:
            ti = self.getmember(cast(str, member))
        return self._extractfile_ti(ti)

    def _extractfile_ti(self, ti: TarInfo) -> BytesIO:
        if ti.isreg() or not _is_supported_type(ti.type):
            start: int = ti.offset_data
            end: int = start + ti.size
            if end > len_bytes(self._data):
                raise ReadError("unexpected end of data")
            return BytesIO(self._data[start:end])
        if ti.islnk() or ti.issym():
            tgt: TarInfo = self._find_link_target(ti)
            return self._extractfile_ti(tgt)
        return BytesIO(b"")

    def _find_link_target(self, ti: TarInfo) -> TarInfo:
        nm: str = ti.name
        depth: int = 0
        cur: TarInfo = ti
        while cur.islnk() or cur.issym():
            nm = cur.linkname
            depth += 1
            if depth > 32:
                raise ExtractError("link cycle detected")
            cur = self.getmember(nm)
        return cur

    def __iter__(self) -> list[TarInfo]:
        return self.getmembers()

    # ── 写接口 ──────────────────────────────────────────────────
    def addfile(self, tarinfo: TarInfo, fileobj: file = None) -> None:
        self._check("awx")
        if fileobj is None and tarinfo.isreg() and tarinfo.size != 0:
            raise ValueError("fileobj not provided for non zero-size regular file")
        buf: bytes = tarinfo.tobuf(self.format, self.encoding, self.errors)
        self._wparts.append(buf)
        self.offset += len_bytes(buf)
        if fileobj is not None:
            chunk: bytes = cast(bytes, fileobj.read(tarinfo.size))
            if len_bytes(chunk) != tarinfo.size:
                raise ReadError("unexpected end of data")
            self._wparts.append(chunk)
            rem: int = tarinfo.size % BLOCKSIZE
            if rem > 0:
                self._wparts.append(NUL * (BLOCKSIZE - rem))
            self.offset += _roundup(tarinfo.size, BLOCKSIZE)
        self.members.append(tarinfo)

    def addfile_bytes(self, tarinfo: TarInfo, data: bytes) -> None:
        """pa 便利口：直接给字节（CPython 走 fileobj）。"""
        self.addfile(tarinfo, BytesIO(data))

    def gettarinfo(self, name: str = "", arcname: str = "") -> TarInfo:
        """按 lstat 建 TarInfo（uid/gid 恒 0，无 uname/gname，见模块文档）。"""
        self._check("awx")
        an: str = arcname
        if len(an) == 0:
            an = name
        an = an.replace(os.sep, "/")
        while an.startswith("/"):
            an = an[1:]
        st: list[int] = _stat_path(name, self.dereference)
        stmd: int = st[0]
        kind: int = st[6]
        typeflag: bytes = REGTYPE
        linkname: str = ""
        if (kind & 4) != 0 and not self.dereference:
            typeflag = SYMTYPE
            linkname = _readlink_path(name)
        elif (kind & 2) != 0:
            typeflag = DIRTYPE
        elif (kind & 1) != 0:
            typeflag = REGTYPE
        else:
            raise ValueError("unsupported file type: " + repr(name))
        ti: TarInfo = TarInfo()
        ti.name = an
        ti.mode = stmd
        ti.uid = 0
        ti.gid = 0
        if typeflag == REGTYPE:
            ti.size = st[5]
        else:
            ti.size = 0
        ti.mtime = st[1]
        ti.type = typeflag
        ti.linkname = linkname
        return ti

    def add(self, name: str, arcname: str = "", recursive: bool = True) -> None:
        """把磁盘上的文件/目录（递归）加入归档。"""
        self._check("awx")
        an: str = arcname
        if len(an) == 0:
            an = name
        if len(self.name) > 0 and _osp.abspath(name) == _osp.abspath(self.name):
            return
        ti: TarInfo = self.gettarinfo(name, an)
        if ti.isreg():
            data: bytes = _gzipio.read_bin(name)
            self.addfile(ti, BytesIO(data))
        elif ti.isdir():
            self.addfile(ti)
            if recursive:
                entries: list[str] = sorted(os.listdir(name))
                for e in entries:
                    self.add(_osp.join(name, e), _osp.join(an, e), recursive)
        else:
            self.addfile(ti)

    # ── 解压到磁盘 ──────────────────────────────────────────────
    def _resolve_member(self, member: object) -> TarInfo:
        if isinstance(member, TarInfo):
            return cast(TarInfo, member)
        return self.getmember(cast(str, member))

    def extract(self, member: object, path: str = "", set_attrs: bool = True) -> None:
        self._check("r")
        ti: TarInfo = self._resolve_member(member)
        base: str = path
        if len(base) == 0:
            base = "."
        target: str = _osp.join(base, _safe_name(ti.name))
        self._extract_member(ti, target, set_attrs)

    def extractall(self, path: str = ".", members: object = None) -> None:
        self._check("r")
        dirs: list[TarInfo] = []
        base: str = path
        if len(base) == 0:
            base = "."
        chosen: list[TarInfo] = []
        if members is None:
            chosen = self.members
        else:
            ms: list[object] = list(members)
            for m in ms:
                chosen.append(self._resolve_member(m))
        for ti in chosen:
            target: str = _osp.join(base, _safe_name(ti.name))
            if ti.isdir():
                # 目录属性延迟到全部解完再补（解子项会重置 mtime）。
                dirs.append(ti)
                self._extract_member(ti, target, False)
            else:
                self._extract_member(ti, target, True)
        i: int = len(dirs) - 1
        while i >= 0:
            d: TarInfo = dirs[i]
            dp: str = _osp.join(base, _safe_name(d.name))
            if _osp.isdir(dp):
                self._chmod(d, dp)
                self._utime(d, dp)
            i -= 1

    def _extract_member(self, ti: TarInfo, targetpath: str, set_attrs: bool) -> None:
        tp: str = targetpath.rstrip("/")
        tp = tp.replace("/", os.sep)
        upper: str = _osp.dirname(tp)
        if len(upper) > 0 and not _osp.exists(upper):
            os.makedirs(upper, exist_ok=True)
        if ti.isreg() or not _is_supported_type(ti.type):
            self._makefile(ti, tp)
        elif ti.isdir():
            self._makedir(ti, tp)
        elif ti.issym() or ti.islnk():
            self._makelink(ti, tp)
        else:
            self._makefile(ti, tp)
        if set_attrs:
            if not ti.issym():
                self._chmod(ti, tp)
                self._utime(ti, tp)

    def _makefile(self, ti: TarInfo, targetpath: str) -> None:
        start: int = ti.offset_data
        end: int = start + ti.size
        if end > len_bytes(self._data):
            raise ReadError("unexpected end of data")
        _gzipio.write_bin(targetpath, self._data[start:end])

    def _makedir(self, ti: TarInfo, targetpath: str) -> None:
        try:
            os.mkdir(targetpath)
        except FileExistsError:
            if not _osp.isdir(targetpath):
                raise

    def _makelink(self, ti: TarInfo, targetpath: str) -> None:
        if ti.issym():
            if _osp.islink(targetpath) or _osp.exists(targetpath):
                os.unlink(targetpath)
            rc: int = _sc.symlink(ti.linkname, targetpath)
            if rc != 0:
                _os_error("symlink", rc, targetpath)
            return
        # 硬链接（LNKTYPE）：pa 没有 os.link，按 CPython 的「链接建不出来
        # 就复制内容」回退口径落成普通文件。
        tgt: TarInfo = self._find_link_target(ti)
        if _osp.islink(targetpath) or _osp.exists(targetpath):
            os.unlink(targetpath)
        self._makefile(tgt, targetpath)

    def _chmod(self, ti: TarInfo, targetpath: str) -> None:
        os.chmod(targetpath, ti.mode & 0o7777)

    def _utime(self, ti: TarInfo, targetpath: str) -> None:
        rc: int = _sc.utime(targetpath, ti.mtime, 0, ti.mtime, 0, 1)
        if rc != 0:
            _os_error("utime", rc, targetpath)

    # ── 收尾 ────────────────────────────────────────────────────
    def close(self) -> None:
        if self.closed:
            return
        self.closed = True
        if self.mode == "r":
            return
        self._wparts.append(NUL * (BLOCKSIZE * 2))
        self.offset += BLOCKSIZE * 2
        rem: int = self.offset % RECORDSIZE
        if rem > 0:
            self._wparts.append(NUL * (RECORDSIZE - rem))
        payload: bytes = b""
        for p in self._wparts:
            payload = payload + p
        if self._comp == "gz":
            try:
                payload = _gzip.compress(payload, self._compresslevel)
            except OSError as e:
                raise CompressionError("gzip compression failed")
        if self.fileobj is not None:
            self.fileobj.write(cast(str, payload))
            self.fileobj.flush()
        elif len(self.name) > 0:
            _gzipio.write_bin(self.name, payload)

    def __enter__(self) -> "TarFile":
        return self

    def __exit__(self, exc_type: object, exc_val: object, exc_tb: object) -> bool:
        self.close()
        return False


def _parse_pax(payload: bytes) -> dict[str, str]:
    """pax 扩展头（"<len> key=value\n" 记录）-> 字典。"""
    hdrs: dict[str, str] = {}
    blob: str = payload.decode("utf-8", "replace")
    while len(blob) > 0:
        sp: int = blob.find(" ")
        if sp <= 0:
            break
        rec_len: int = 0
        try:
            rec_len = int(blob[:sp])
        except ValueError:
            break
        if rec_len <= 0 or rec_len > len(blob):
            break
        rec: str = blob[sp + 1:rec_len - 1]
        eq: int = rec.find("=")
        if eq > 0:
            hdrs[rec[:eq]] = rec[eq + 1:]
        blob = blob[rec_len:]
    return hdrs


def _finish_header(h: TarInfo, nm: str, ln: str, pax: dict[str, str]) -> TarInfo:
    """把累积的长名/长链接/pax 字段落到真成员头上（pax 优先级最高）。"""
    if len(nm) > 0:
        h.name = nm
    if len(ln) > 0:
        h.linkname = ln
    if len(pax) > 0:
        _apply_pax(h, pax)
    return h


def _apply_pax(ti: TarInfo, hdrs: dict[str, str]) -> None:
    if "path" in hdrs:
        ti.name = hdrs["path"]
    if "linkpath" in hdrs:
        ti.linkname = hdrs["linkpath"]
    if "size" in hdrs:
        try:
            ti.size = int(hdrs["size"])
        except ValueError:
            pass
    if "mtime" in hdrs:
        try:
            ti.mtime = int(float(hdrs["mtime"]))
        except ValueError:
            pass
    if "uid" in hdrs:
        try:
            ti.uid = int(hdrs["uid"])
        except ValueError:
            pass
    if "gid" in hdrs:
        try:
            ti.gid = int(hdrs["gid"])
        except ValueError:
            pass
    if "uname" in hdrs:
        ti.uname = hdrs["uname"]
    if "gname" in hdrs:
        ti.gname = hdrs["gname"]


def is_tarfile(name: str) -> bool:
    try:
        data: bytes = _gzipio.read_bin(name)
    except OSError:
        return False
    if len_bytes(data) >= 2 and data[0] == 0x1F and data[1] == 0x8B:
        try:
            data = _gzip.decompress(data)
        except OSError:
            return False
        except EOFError:
            return False
    if len_bytes(data) < BLOCKSIZE:
        return False
    try:
        TarInfo.frombuf(data[0:BLOCKSIZE])
        return True
    except TarError:
        return False


def open(name: str = "", mode: str = "r", fileobj: file = None,
         format: int = PAX_FORMAT, encoding: str = "utf-8",
         errors: str = "replace", compresslevel: int = 9) -> "TarFile":
    """打开 tar 归档（r/w/x/a，可带 :gz；fileobj 或路径二选一）。"""
    if len(name) == 0 and fileobj is None:
        raise ValueError("nothing to open")
    return TarFile(name, mode, fileobj, format, encoding, errors, compresslevel)

# ── 文件系统助手（_shutilc 胶水：stat/readlink/symlink/utime）────────
def _os_error(what: str, rc: int, path: str) -> object:
    msg: str = what + ": " + _sc.strerror(rc) + ": " + repr(path)
    if rc == -1:
        raise FileNotFoundError(msg)
    if rc == -2:
        raise FileExistsError(msg)
    if rc == -3:
        raise PermissionError(msg)
    if rc == -4:
        raise NotADirectoryError(msg)
    if rc == -5:
        raise IsADirectoryError(msg)
    raise OSError(msg)


def _stat_path(path: str, follow: bool) -> list[int]:
    """lstat/stat -> [mode, mtime_s, mtime_ns, atime_s, atime_ns, size, kind]。"""
    out: list[int] = [0, 0, 0, 0, 0, 0, 0]
    f: int = 0
    if follow:
        f = 1
    rc: int = _sc.stat(path, f, out)
    if rc != 0:
        _os_error("stat", rc, path)
    return out


def _readlink_path(path: str) -> str:
    cap: int = 4096
    while True:
        buf: bytes = bytes(cap)
        n: int = _sc.readlink(path, buf, cap)
        if n >= 0:
            return buf[:n].decode("utf-8", "replace")
        if n == -1 and cap < 1048576:
            cap = cap * 2
            continue
        _os_error("readlink", n, path)
        return ""


def _safe_name(name: str) -> str:
    """成员名净化：剥前导 '/'、丢 ''/'.'/'..' 段（同 zipfile 口径）。"""
    parts: list[str] = []
    for p in name.split("/"):
        if p == "" or p == "." or p == "..":
            continue
        parts.append(p)
    return "/".join(parts)
