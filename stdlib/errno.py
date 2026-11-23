"""errno —— POSIX 错误码常量（纯 pyadamas 源码）。

值与本机 CPython 3.14（Linux x86_64）逐一对拍；均为 int。
EWOULDBLOCK 与 EAGAIN 在 Linux 上同值（11），与 CPython 一致。

errorcode: dict[int, str] —— CPython errno.errorcode 的子集，仅收录本模块
导出的错误码（反向查名字）；常量表里能用 dict 构造，故保留。
"""

EPERM = 1
ENOENT = 2
ESRCH = 3
EINTR = 4
EIO = 5
ENOMEM = 12
EACCES = 13
EFAULT = 14
ENOTDIR = 20
EISDIR = 21
EINVAL = 22
EMFILE = 24
ENOSPC = 28
EDOM = 33
ERANGE = 34
EAGAIN = 11
ENOEXEC = 8
EEXIST = 17
EPIPE = 32
ETIMEDOUT = 110

# Linux 上 EWOULDBLOCK == EAGAIN（CPython 也是同值）。
EWOULDBLOCK = EAGAIN

errorcode = {
    1: "EPERM",
    2: "ENOENT",
    3: "ESRCH",
    4: "EINTR",
    5: "EIO",
    8: "ENOEXEC",
    11: "EAGAIN",
    12: "ENOMEM",
    13: "EACCES",
    17: "EEXIST",
    20: "ENOTDIR",
    21: "EISDIR",
    22: "EINVAL",
    24: "EMFILE",
    28: "ENOSPC",
    32: "EPIPE",
    33: "EDOM",
    34: "ERANGE",
    110: "ETIMEDOUT",
}
