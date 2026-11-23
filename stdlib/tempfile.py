"""tempfile 模块的 pyadamas 子集（纯 pyadamas 源码 + 独占创建 C 胶水）。

覆盖：mkstemp / mkdtemp / mktemp / NamedTemporaryFile / TemporaryDirectory /
gettempdir / gettempprefix / tempdir 全局；TMP_MAX。
mkstemp 走 C 胶水 open(O_CREAT|O_EXCL|O_RDWR, 0600)（同 CPython 语义），
随机名 8 字符 [a-z0-9_]（同 CPython 字符集）；mkdtemp 用 os.mkdir 的
FileExistsError 重试（errno 映射是 stdlib2 使能）。

语义边界（轻量标尺，见 docs/stdlib-batch2.md 与 docs/stdlib-gaps.md 第 8 节）：
- mkstemp 返回 (fd, 绝对路径)；fd 用 open(fd, mode) 包装（os.fdopen 的
  替代，closefd 默认 True），裸 fd 用 os.close(fd)（stdlib2 新增）。
- NamedTemporaryFile 的 newline 只支持 None（open 降级的字面量限制）；
  encoding/errors 文本模式可用。delete=True 在 close() 时删文件；
  没有 __del__ 终结器——忘记 close 会留下文件（CPython 有弱引用终结）。
- TemporaryDirectory 隐式清理同上：用 with 或显式 cleanup()。
- TemporaryFile / SpooledTemporaryFile 不做（O_TMPFILE/卷 spill），桥接。
- tempdir 全局默认空串（CPython 是 None；None/0 同形边界），设值后
  gettempdir 优先用它。
- mktemp 用 mkstemp 创建后立即 close+unlink 再返回名字（同 CPython 的
  「不占用文件」语义，但同样有 TOCTOU 竞态；CPython 已标记 deprecated）。
- 随机名不与 CPython 逐字一致（随机源不同）；差分只钉前缀/后缀/存在性/
  权限/内容等功能属性。
"""

import os
import os.path as _osp
import random as _random
import shutil as _shutil
cimport _tempfilec as _c
from typing import cast


TMP_MAX: int = 10000

tempdir: str = ""

_chars: str = "abcdefghijklmnopqrstuvwxyz0123456789_"


def gettempprefix() -> str:
    return "tmp"


def gettempdir() -> str:
    global tempdir
    if len(tempdir) > 0:
        return tempdir
    candidates: list[str] = []
    for name in ["TMPDIR", "TEMP", "TMP"]:
        v = os.getenv(name)
        if len(v) > 0:
            candidates.append(v)
    candidates.append("/tmp")
    candidates.append("/var/tmp")
    candidates.append("/usr/tmp")
    for d in candidates:
        if _osp.isdir(d) and os.access(d, os.W_OK | os.X_OK):
            tempdir = d
            return d
    raise FileNotFoundError("No usable temporary directory found")


def _resolve_dir(dir: object) -> str:
    if dir is None:
        return gettempdir()
    s: str = dir
    return s


def _norm_suffix(suffix: object) -> str:
    if suffix is None:
        return ""
    s: str = suffix
    return s


def _norm_prefix(prefix: object) -> str:
    if prefix is None:
        return "tmp"
    s: str = prefix
    return s


def _random_name() -> str:
    out = ""
    i = 0
    while i < 8:
        out = out + _chars[_random.getrandbits(8) % 37]
        i += 1
    return out


def _raise_os(what: str, rc: int, path: str) -> None:
    msg = what + ": " + _c.strerror(rc) + ": " + repr(path)
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


def mkdtemp(suffix: object = None, prefix: object = None, dir: object = None) -> str:
    d = _resolve_dir(dir)
    s = _norm_suffix(suffix)
    p = _norm_prefix(prefix)
    i = 0
    while i < TMP_MAX:
        name = _osp.join(d, p + _random_name() + s)
        try:
            os.mkdir(name)
            return name
        except FileExistsError:
            i += 1
            continue
    raise FileExistsError("No usable temporary directory name found")


def mkstemp(suffix: object = None, prefix: object = None, dir: object = None) -> tuple[int, str]:
    d = _resolve_dir(dir)
    s = _norm_suffix(suffix)
    p = _norm_prefix(prefix)
    i = 0
    while i < TMP_MAX:
        name = _osp.join(d, p + _random_name() + s)
        fd = _c.open_excl(name)
        if fd >= 0:
            return (fd, _osp.abspath(name))
        if fd == -2:
            i += 1
            continue
        _raise_os("mkstemp", fd, name)
    raise FileExistsError("No usable temporary file name found")
    return (-1, "")


def mktemp(suffix: object = None, prefix: object = None, dir: object = None) -> str:
    """返回一个临时文件路径但不创建文件（CPython deprecated 的同名 API）。

    实现：mkstemp 独占创建 → os.close(fd) → os.unlink(path) → 返回 path。
    与 CPython 一样存在 TOCTOU 竞态，不用于安全场景。
    """
    fd, path = mkstemp(suffix, prefix, dir)
    os.close(fd)
    os.unlink(path)
    return path


class _TemporaryFileWrapper:
    """NamedTemporaryFile 的包装（代理底层 file，close 时按 delete 删文件）。"""
    name: str
    delete: bool
    file: file
    _closed: bool

    def __init__(self, f: file, name: str, delete: bool) -> None:
        self.file = f
        self.name = name
        self.delete = delete
        self._closed = False

    def write(self, data: object) -> int:
        s: str = data
        return self.file.write(s)

    def writelines(self, lines: list[str]) -> None:
        self.file.writelines(lines)

    def read(self, size: int = -1) -> str:
        return self.file.read(size)

    def readline(self, size: int = -1) -> str:
        return self.file.readline(size)

    def readlines(self) -> list[str]:
        return self.file.readlines()

    def seek(self, off: int, whence: int = 0) -> int:
        return self.file.seek(off, whence)

    def tell(self) -> int:
        return self.file.tell()

    def truncate(self, size: int = -1) -> int:
        return self.file.truncate(size)

    def flush(self) -> None:
        self.file.flush()

    def fileno(self) -> int:
        return self.file.fileno()

    def isatty(self) -> bool:
        return self.file.isatty()

    def readable(self) -> bool:
        return self.file.readable()

    def writable(self) -> bool:
        return self.file.writable()

    def seekable(self) -> bool:
        return self.file.seekable()

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        self.file.close()
        if self.delete:
            try:
                os.unlink(self.name)
            except FileNotFoundError:
                pass

    def __iter__(self) -> list[str]:
        return self.file.readlines()

    def __enter__(self) -> "_TemporaryFileWrapper":
        return self

    def __exit__(self, exc_type: object, exc_val: object, exc_tb: object) -> bool:
        self.close()
        return False


def NamedTemporaryFile(mode: str = "w+b", buffering: int = -1, encoding: object = None,
                       newline: object = None, suffix: object = None, prefix: object = None,
                       dir: object = None, delete: bool = True,
                       errors: object = None) -> _TemporaryFileWrapper:
    if not (newline is None):
        raise NotImplementedError("NamedTemporaryFile newline= is not supported (use None)")
    fd, name = mkstemp(suffix, prefix, dir)
    f = open(fd, mode, buffering, encoding, errors)
    return _TemporaryFileWrapper(f, name, delete)


class TemporaryDirectory:
    """临时目录上下文管理（with 退出或 cleanup() 时 rmtree）。"""
    name: str
    _ignore_cleanup_errors: bool
    _closed: bool

    def __init__(self, suffix: object = None, prefix: object = None, dir: object = None,
                 ignore_cleanup_errors: bool = False) -> None:
        self.name = mkdtemp(suffix, prefix, dir)
        self._ignore_cleanup_errors = ignore_cleanup_errors
        self._closed = False

    def cleanup(self) -> None:
        if self._closed:
            return
        self._closed = True
        _shutil.rmtree(self.name, ignore_errors=self._ignore_cleanup_errors)

    def __enter__(self) -> str:
        return self.name

    def __exit__(self, exc_type: object, exc_val: object, exc_tb: object) -> bool:
        self.cleanup()
        return False
