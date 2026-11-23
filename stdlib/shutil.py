"""shutil 模块的 pyadamas 子集（纯 pyadamas 源码 + stat/utime/symlink C 胶水）。

覆盖：copyfile / copymode / copystat / copy / copy2 / move / rmtree /
copytree / ignore_patterns / which / disk_usage；Error / SameFileError /
SpecialFileError / DiskUsage。
二进制 IO 走一批验证过的字节直通口径（open rb/wb + str 承载原始字节）。

语义边界（轻量标尺，见 docs/stdlib-batch2.md）：
- copyfile 分块读写（64KB），不检查特殊文件（fifo 等）；follow_symlinks=False
  时源是符号链接则复制链接本身。
- copystat 复制权限位 + atime/mtime（纳秒）；不复制 flags（UF_IMMUTABLE 等）。
  Linux chmod 不跟随符号链接的口径不建模（follow_symlinks 对 chmod 不生效）。
- move 的 copy_function 参数不做（固定 copy2）；跨设备回退 copy+delete。
- rmtree 是朴素递归（非 fd-based 防竞态口径）；onerror/onexc 参数不做，
  ignore_errors 可用；符号链接不跟随（安全口径同 CPython）。
- copytree 错误聚合成 Error（消息是合并文本，不是 CPython 的 (src,dst,why)
  列表 repr）；ignore 收可调用（ignore_patterns 或自定义 (path, names) 函数）。
- which 只查 PATH（os.getenv），不查 PATHEXT（POSIX 口径）；mode 默认
  F_OK|X_OK（=1，字面量默认值限制）。
- disk_usage 用 subprocess 调 `df -k` 解析（PA 的 os 没有 statvfs，
  也没有 C 胶水权限加原语）；返回 DiskUsage 类实例（有 total/used/free
  属性 + 下标 0/1/2 + 3 元组解包等价）。数值是 1024 字节块换算，与
  CPython os.statvfs 口径一致但由于 df 报的是挂载点级统计，对小路径
  （tmpfs 子目录）结果应与 CPython 相同。get_terminal_size /
  make_archive / unpack_archive 不做，长尾用 from python import shutil 桥接。
- 错误消息合理即可，不逐字对齐 CPython。
"""

import os
import subprocess
import os.path as _osp
import fnmatch as _fnmatch
cimport _shutilc as _c
from typing import cast


class Error(Exception):
    pass


class SameFileError(Error):
    pass


class SpecialFileError(Error):
    pass


_BUFSIZE: int = 65536


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


def _stat(path: str, follow: bool) -> list[int]:
    out: list[int] = [0, 0, 0, 0, 0, 0, 0]
    f = 0
    if follow:
        f = 1
    rc = _c.stat(path, f, out)
    if rc != 0:
        _raise_os("stat", rc, path)
    return out


def _islink(path: str) -> bool:
    st = _stat(path, False)
    return (st[6] & 4) != 0


def _readlink(path: str) -> str:
    cap = 4096
    while True:
        buf = bytes(cap)
        n = _c.readlink(path, buf, cap)
        if n >= 0:
            return buf[:n].decode("utf-8", "replace")
        if n == -1 and cap < 1048576:
            cap = cap * 2
            continue
        _raise_os("readlink", n, path)
        return ""


def _copylink(src: str, dst: str) -> None:
    target = _readlink(src)
    rc = _c.symlink(target, dst)
    if rc != 0:
        _raise_os("symlink", rc, dst)


def _samefile(a: str, b: str) -> bool:
    try:
        return _osp.samefile(a, b)
    except OSError:
        return False


def copyfile(src: str, dst: str, *, follow_symlinks: bool = True) -> str:
    if _samefile(src, dst):
        raise SameFileError(repr(src) + " and " + repr(dst) + " are the same file")
    if not follow_symlinks and _islink(src):
        _copylink(src, dst)
        return dst
    # 单层 try/finally：嵌套 try/finally 之间抛异常会撞基线的临时栈 mark
    # bug（docs/stdlib-batch2.md 已知问题，ehx.py 最小复现）；fdst 在异常
    # 路径由帧 RC 清理兜底，正常路径显式 close。
    fsrc = open(src, "rb")
    try:
        fdst = open(dst, "wb")
        while True:
            chunk = fsrc.read(_BUFSIZE)
            if len(chunk) == 0:
                break
            fdst.write(chunk)
        fdst.close()
    finally:
        fsrc.close()
    return dst


def copymode(src: str, dst: str, *, follow_symlinks: bool = True) -> None:
    st = _stat(src, follow_symlinks)
    os.chmod(dst, st[0] & 0o7777)


def copystat(src: str, dst: str, *, follow_symlinks: bool = True) -> None:
    st = _stat(src, follow_symlinks)
    os.chmod(dst, st[0] & 0o7777)
    f = 0
    if follow_symlinks:
        f = 1
    rc = _c.utime(dst, st[3], st[4], st[1], st[2], f)
    if rc != 0:
        _raise_os("utime", rc, dst)


def copy(src: str, dst: str, *, follow_symlinks: bool = True) -> str:
    if _osp.isdir(dst):
        dst = _osp.join(dst, _osp.basename(src))
    copyfile(src, dst, follow_symlinks=follow_symlinks)
    copymode(src, dst, follow_symlinks=follow_symlinks)
    return dst


def copy2(src: str, dst: str, *, follow_symlinks: bool = True) -> str:
    if _osp.isdir(dst):
        dst = _osp.join(dst, _osp.basename(src))
    copyfile(src, dst, follow_symlinks=follow_symlinks)
    copystat(src, dst, follow_symlinks=follow_symlinks)
    return dst


def _destinsrc(src: str, dst: str) -> bool:
    s = _osp.abspath(src)
    d = _osp.abspath(dst)
    if s == d:
        return True
    if not s.endswith("/"):
        s = s + "/"
    return d.startswith(s)


def move(src: str, dst: str) -> str:
    real_dst = dst
    if _osp.isdir(dst):
        real_dst = _osp.join(dst, _osp.basename(src))
        if _osp.exists(real_dst):
            raise Error("Destination path '" + real_dst + "' already exists")
    try:
        os.rename(src, real_dst)
    except OSError:
        if _islink(src):
            _copylink(src, real_dst)
            os.unlink(src)
        elif _osp.isdir(src):
            if _destinsrc(src, dst):
                raise Error("Cannot move a directory '" + src + "' into itself '" + dst + "'.")
            copytree(src, real_dst, symlinks=True)
            rmtree(src)
        else:
            copy2(src, real_dst)
            os.unlink(src)
    return real_dst


def rmtree(path: str, ignore_errors: bool = False) -> None:
    try:
        if _islink(path):
            raise OSError("Cannot call rmtree on a symbolic link")
        names = os.listdir(path)
    except OSError:
        if ignore_errors:
            return
        raise
    for name in names:
        full = _osp.join(path, name)
        try:
            if _islink(full) or not _osp.isdir(full):
                os.unlink(full)
            else:
                rmtree(full, ignore_errors)
        except OSError:
            if not ignore_errors:
                raise
    try:
        os.rmdir(path)
    except OSError:
        if not ignore_errors:
            raise


def ignore_patterns(*patterns: str) -> object:
    """返回闭包 (path, names) -> list[str]（同 CPython 的可调用口径）。

    用闭包而不是 __call__ 类实例：unknown 上的类实例间接调用目前会跳进
    vtable 数据（基线已存在，见 docs/stdlib-batch2.md 已知问题），函数值
    间接调用是一批验证过的路径。"""
    pats: list[str] = []
    for p in patterns:
        pats.append(p)

    def _ignore(path: str, names: list[str]) -> list[str]:
        out: list[str] = []
        for n in names:
            for p in pats:
                if _fnmatch.fnmatch(n, p):
                    out.append(n)
                    break
        return out

    return _ignore


def copytree(src: str, dst: str, symlinks: bool = False, ignore: object = None,
             *, dirs_exist_ok: bool = False) -> str:
    names = os.listdir(src)
    ignored: list[str] = []
    if not (ignore is None):
        ig: object = ignore(src, names)
        ignored = list(ig)
    os.makedirs(dst, exist_ok=dirs_exist_ok)
    errors: list[str] = []
    for name in names:
        if name in ignored:
            continue
        s = _osp.join(src, name)
        d = _osp.join(dst, name)
        try:
            if _islink(s):
                if symlinks:
                    _copylink(s, d)
                else:
                    copy2(s, d)
            elif _osp.isdir(s):
                copytree(s, d, symlinks, ignore, dirs_exist_ok=dirs_exist_ok)
            else:
                copy2(s, d)
        except Error as e:
            errors.append(str(e))
        except OSError as e:
            errors.append(str(e))
    if len(errors) > 0:
        raise Error("copytree errors: " + "; ".join(errors))
    return dst


def which(cmd: str, mode: int = 1, path: object = None) -> object:
    if len(_osp.dirname(cmd)) > 0:
        if _osp.isfile(cmd) and os.access(cmd, mode):
            return cmd
        return None
    p: str = ""
    if path is None:
        p = os.getenv("PATH")
    else:
        p = path
    if len(p) == 0:
        return None
    for d in p.split(":"):
        dd = d
        if len(dd) == 0:
            dd = "."
        full = _osp.join(dd, cmd)
        if _osp.isfile(full) and os.access(full, mode):
            return full
    return None


class DiskUsage:
    """disk_usage 的返回值：total/used/free 属性 + 下标访问（同 namedtuple 口径）。"""
    total: int
    used: int
    free: int

    def __init__(self, total: int, used: int, free: int) -> None:
        self.total = total
        self.used = used
        self.free = free

    def __len__(self) -> int:
        return 3

    def __getitem__(self, i: int) -> int:
        if i == 0:
            return self.total
        if i == 1:
            return self.used
        if i == 2:
            return self.free
        raise IndexError("tuple index out of range")

    def __repr__(self) -> str:
        return "usage(total=" + str(self.total) + ", used=" + str(self.used) + ", free=" + str(self.free) + ")"


def disk_usage(path: str) -> DiskUsage:
    """返回路径所在文件系统的磁盘用量（total/used/free，单位字节）。

    用 `df -k <path>` 解析（POSIX 口径）：第 2 行是数据，字段 1/2/3 是
    1K-blocks/Used/Available。CPython 用 os.statvfs，结果应与 df 一致。
    """
    r = subprocess.run(["df", "-k", path], capture_output=True, text=True)
    if r.returncode != 0:
        raise OSError("disk_usage failed for " + repr(path))
    lines = r.stdout.split("\n")
    if len(lines) < 2:
        raise OSError("disk_usage: no data for " + repr(path))
    data = lines[1].split()
    if len(data) < 4:
        raise OSError("disk_usage: bad df output for " + repr(path))
    total = int(data[1]) * 1024
    used = int(data[2]) * 1024
    free = int(data[3]) * 1024
    return DiskUsage(total, used, free)
