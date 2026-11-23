"""getpass —— 用户名探测（纯 pyadamas 源码）。

getuser() 按 CPython getpass.getuser 的顺序读环境变量
USER / LOGNAME / USERNAME，取到非空值即返回；都拿不到返回 "unknown"
（CPython 在 posix 上会继续查 pwd 模块，PyAdamas 没有 pwd，故降级为
"unknown"）。

CPython 会在环境变量为空串时也继续往下找；os.getenv 在 PyAdamas 里
缺失统一返回空串，因此空串等价于「没拿到」，语义一致。
"""


import os


def getuser() -> str:
    """返回当前用户名：USER > LOGNAME > USERNAME > "unknown"。"""
    u = os.getenv("USER")
    if u != "":
        return u
    u = os.getenv("LOGNAME")
    if u != "":
        return u
    u = os.getenv("USERNAME")
    if u != "":
        return u
    return "unknown"
