"""stat —— 文件类型/权限位常量与 S_IS* 判定（纯 pyadamas 源码）。

S_IFMT 在 CPython 里是函数（S_IFMT(mode) 取类型位）。但 PyAdamas 的
类型检查不支持「常量既是 int 又能当函数调用」，因此这里按 POSIX 定义
把 S_IFMT 暴露为掩码常量 0o170000（61440），判定函数用 mode & S_IFMT
完成；S_IFMT(x) 的调用形式在 CPython 里返回 x & 0o170000，本模块不建议
按函数调用（需要时自己写 mode & S_IFMT）。

其余常量与 S_IS* 判定与 CPython 3.14（Linux x86_64）逐一对拍。
"""

S_IFMT = 61440
S_IFDIR = 16384
S_IFREG = 32768
S_IFLNK = 40960
S_IFIFO = 4096
S_IFSOCK = 49152

S_IRUSR = 256
S_IWUSR = 128
S_IXUSR = 64
S_IRGRP = 32
S_IWGRP = 16
S_IXGRP = 8
S_IROTH = 4
S_IWOTH = 2
S_IXOTH = 1


def S_ISDIR(mode: int) -> bool:
    return (mode & S_IFMT) == S_IFDIR


def S_ISREG(mode: int) -> bool:
    return (mode & S_IFMT) == S_IFREG


def S_ISLNK(mode: int) -> bool:
    return (mode & S_IFMT) == S_IFLNK


def S_ISFIFO(mode: int) -> bool:
    return (mode & S_IFMT) == S_IFIFO


def S_ISSOCK(mode: int) -> bool:
    return (mode & S_IFMT) == S_IFSOCK
