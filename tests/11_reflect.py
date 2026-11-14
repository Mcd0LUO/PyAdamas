# 反射：非类接收者（模块/unknown）的 hasattr/getattr 在 check 阶段放过
# （静态证明不了有还是无，交给 build 决定）。这是 CPython 里最常见的
# 动态判别，标准库一半文件都在用它。
import os


def probe(q: int) -> int:
    a = hasattr(os, "fork")
    b = getattr(q, "x", 0)
    return 0 if a and b == 0 else 1
