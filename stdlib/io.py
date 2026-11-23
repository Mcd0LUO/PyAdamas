"""io —— 内存文本/二进制流（P4 块2，纯 pyadamas 源码）。

StringIO/BytesIO 实现 file 协议面（read/readline/readlines/write/
writelines/seek/tell/truncate/flush/close/getvalue + closed 属性 +
with 协议）。门控 PYADAMAS_LANG_GAPS=1 时 file 标注收 FileLike
（有 read/write 方法面的类实例），csv.writer/reader 等经运行期
pa_filelike_* 分派直接收 StringIO/BytesIO。

语义边界（轻量标尺）：
- pa 的 str/bytes 是不可变码点序列：write 用切片拼接重建缓冲，
  O(n) 每次写入（CPython 是摊销 O(1)）；功能口径一致。
- bytes 与 str 共用运行期表示，但类型检查器会拦：BytesIO.write(str)
  在 PA 是编译期错误（expected bytes, got str），CPython 是运行期
  TypeError；两者都不接受，只是报错时机不同。
- 不支持 detach/getbuffer/readinto（pa 没有可写缓冲原语）。
- truncate(size) 显式负参按默认处理（用当前 pos）：pa 的 int 默认参
  只能是字面量，None 默认与显式 0 在 int 槽里不可分（既有 pa 限制），
  无法复刻 CPython「truncate() 用 pos、truncate(-1) 抛 ValueError」。
  truncate() / truncate(非负) 与 CPython 一致。
- newline 参数不做翻译（StringIO 默认 newline="\n" 口径：写入原样，
  读取按 \n 分行；CPython 的 \r\n 翻译不做）。
"""


class StringIO:
    """文本内存流。初始值可选；write 从当前 pos 覆盖/追加。"""

    _buf: str
    _pos: int
    _closed: bool

    def __init__(self, initial_value: str = "") -> None:
        self._buf = initial_value
        self._pos = 0
        self._closed = False

    def _check(self) -> None:
        if self._closed:
            raise ValueError("I/O operation on closed file")

    @property
    def closed(self) -> bool:
        return self._closed

    def read(self, size: int = -1) -> str:
        self._check()
        n: int = len(self._buf)
        if self._pos >= n:
            return ""
        end: int = n
        if size >= 0 and self._pos + size < n:
            end = self._pos + size
        s: str = self._buf[self._pos:end]
        self._pos = end
        return s

    def readline(self, size: int = -1) -> str:
        self._check()
        n: int = len(self._buf)
        if self._pos >= n:
            return ""
        nl: int = self._buf.find("\n", self._pos)
        end: int = n
        if nl >= 0:
            end = nl + 1
        if size >= 0 and self._pos + size < end:
            end = self._pos + size
        s: str = self._buf[self._pos:end]
        self._pos = end
        return s

    def readlines(self, hint: int = -1) -> list[str]:
        self._check()
        out: list[str] = []
        total: int = 0
        while True:
            line: str = self.readline()
            if len(line) == 0:
                break
            out.append(line)
            total += len(line)
            if hint >= 0 and total >= hint:
                break
        return out

    def write(self, s: str) -> int:
        self._check()
        n: int = len(s)
        if n == 0:
            return 0
        buf: str = self._buf
        bl: int = len(buf)
        if self._pos > bl:
            buf = buf + "\x00" * (self._pos - bl)
            bl = self._pos
        if self._pos >= bl:
            self._buf = buf + s
        else:
            end: int = self._pos + n
            if end > bl:
                end = bl
            self._buf = buf[:self._pos] + s + buf[end:]
        self._pos += n
        return n

    def writelines(self, lines: list[str]) -> None:
        self._check()
        for line in lines:
            self.write(line)

    def seek(self, offset: int, whence: int = 0) -> int:
        self._check()
        # CPython StringIO/BytesIO 口径：whence 1/2 只接受 offset 0
        # （非零 cur/end 相对寻址报 OSError，消息逐字一致）。
        if whence != 0 and whence != 1 and whence != 2:
            raise ValueError("Invalid whence (" + str(whence) + ", should be 0, 1 or 2)")
        if whence != 0 and offset != 0:
            raise OSError("Can't do nonzero cur-relative seeks")
        base: int = 0
        if whence == 1:
            base = self._pos
        elif whence == 2:
            base = len(self._buf)
        p: int = base + offset
        if p < 0:
            raise ValueError("Negative seek position " + str(p))
        self._pos = p
        return p

    def tell(self) -> int:
        self._check()
        return self._pos

    def truncate(self, size: int = -1) -> int:
        self._check()
        s: int = self._pos if size < 0 else size
        if s < 0:
            raise ValueError("Negative size value " + str(s))
        if s < len(self._buf):
            self._buf = self._buf[:s]
        return s

    def readable(self) -> bool:
        self._check()
        return True

    def writable(self) -> bool:
        self._check()
        return True

    def seekable(self) -> bool:
        self._check()
        return True

    def flush(self) -> None:
        self._check()

    def close(self) -> None:
        self._closed = True

    def getvalue(self) -> str:
        self._check()
        return self._buf

    def __enter__(self) -> StringIO:
        return self

    def __exit__(self, et, ev, tb) -> bool:
        self.close()
        return False


class BytesIO:
    """二进制内存流。与 StringIO 同口径，缓冲是 bytes。"""

    _buf: bytes
    _pos: int
    _closed: bool

    def __init__(self, initial_value: bytes = b"") -> None:
        self._buf = initial_value
        self._pos = 0
        self._closed = False

    def _check(self) -> None:
        if self._closed:
            raise ValueError("I/O operation on closed file")

    @property
    def closed(self) -> bool:
        return self._closed

    def read(self, size: int = -1) -> bytes:
        self._check()
        n: int = len(self._buf)
        if self._pos >= n:
            return b""
        end: int = n
        if size >= 0 and self._pos + size < n:
            end = self._pos + size
        s: bytes = self._buf[self._pos:end]
        self._pos = end
        return s

    def readline(self, size: int = -1) -> bytes:
        self._check()
        n: int = len(self._buf)
        if self._pos >= n:
            return b""
        nl: int = self._buf.find(b"\n", self._pos)
        end: int = n
        if nl >= 0:
            end = nl + 1
        if size >= 0 and self._pos + size < end:
            end = self._pos + size
        s: bytes = self._buf[self._pos:end]
        self._pos = end
        return s

    def readlines(self, hint: int = -1) -> list[bytes]:
        self._check()
        out: list[bytes] = []
        total: int = 0
        while True:
            line: bytes = self.readline()
            if len(line) == 0:
                break
            out.append(line)
            total += len(line)
            if hint >= 0 and total >= hint:
                break
        return out

    def write(self, s: bytes) -> int:
        self._check()
        n: int = len(s)
        if n == 0:
            return 0
        buf: bytes = self._buf
        bl: int = len(buf)
        if self._pos > bl:
            buf = buf + b"\x00" * (self._pos - bl)
            bl = self._pos
        if self._pos >= bl:
            self._buf = buf + s
        else:
            end: int = self._pos + n
            if end > bl:
                end = bl
            self._buf = buf[:self._pos] + s + buf[end:]
        self._pos += n
        return n

    def writelines(self, lines: list[bytes]) -> None:
        self._check()
        for line in lines:
            self.write(line)

    def seek(self, offset: int, whence: int = 0) -> int:
        self._check()
        # CPython StringIO/BytesIO 口径：whence 1/2 只接受 offset 0
        # （非零 cur/end 相对寻址报 OSError，消息逐字一致）。
        if whence != 0 and whence != 1 and whence != 2:
            raise ValueError("Invalid whence (" + str(whence) + ", should be 0, 1 or 2)")
        if whence != 0 and offset != 0:
            raise OSError("Can't do nonzero cur-relative seeks")
        base: int = 0
        if whence == 1:
            base = self._pos
        elif whence == 2:
            base = len(self._buf)
        p: int = base + offset
        if p < 0:
            raise ValueError("Negative seek position " + str(p))
        self._pos = p
        return p

    def tell(self) -> int:
        self._check()
        return self._pos

    def truncate(self, size: int = -1) -> int:
        self._check()
        s: int = self._pos if size < 0 else size
        if s < 0:
            raise ValueError("Negative size value " + str(s))
        if s < len(self._buf):
            self._buf = self._buf[:s]
        return s

    def readable(self) -> bool:
        self._check()
        return True

    def writable(self) -> bool:
        self._check()
        return True

    def seekable(self) -> bool:
        self._check()
        return True

    def flush(self) -> None:
        self._check()

    def close(self) -> None:
        self._closed = True

    def getvalue(self) -> bytes:
        self._check()
        return self._buf

    def __enter__(self) -> BytesIO:
        return self

    def __exit__(self, et, ev, tb) -> bool:
        self.close()
        return False