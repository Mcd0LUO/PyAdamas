# expect: takes no arguments (no __init__)
# 没有 __init__ 的类带实参实例化：CPython 报 TypeError。
# 以前为了一个不存在的理由（SafeUUID 其实是 Enum）一律放过，静默丢弃实参。
class Empty:
    pass

a = Empty(1, 2, "bug")
