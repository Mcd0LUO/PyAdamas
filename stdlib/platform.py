"""platform —— 平台信息探测（纯 pyadamas 源码子集）。

提供 system() / machine() / release() / python_version()：
- system()：返回 "Linux"。CPython 在 Linux 上通过 os.uname()[0] 得到；
  PyAdamas 没有 os.uname，故按宿主环境（Linux x86_64，本文件随编译器
  分发的目标平台）固定返回。
- machine()：返回 "x86_64"，同上，按目标平台固定。
- release()：优先读环境变量 PYADAMAS_OS_RELEASE（构建/部署时可注入与
  CPython platform.release() 对齐的内核版本）；没有时返回空串 ""。
  CPython 的 platform.release() 来自 os.uname().release，PyAdamas 无
  os.uname，也没有 cimport 之外的 uname 原语，故只能这样兜底。
- python_version()：返回 PyAdamas 的兼容版本号字符串（当前 "3.12.0"）。
  注意：这是 PyAdamas 对外声明的 CPython 兼容版本形态，不是编译器自身
  版本号（编译器版本见 CHANGELOG / CMake 的 PYADAMAS_VERSION）。

没有 os.uname 时就是按上述常量/环境变量兜底，不会抛异常。
"""


import os


def system() -> str:
    """操作系统名，按目标平台固定为 "Linux"。"""
    return "Linux"


def machine() -> str:
    """机器架构名，按目标平台固定为 "x86_64"。"""
    return "x86_64"


def release() -> str:
    """内核版本：读 PYADAMAS_OS_RELEASE，缺失返回空串。"""
    return os.getenv("PYADAMAS_OS_RELEASE")


def python_version() -> str:
    """PyAdamas 的 CPython 兼容版本号（兼容形态，非编译器版本号）。"""
    return "3.12.0"
