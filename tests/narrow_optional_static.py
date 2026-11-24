# `T | None` 窄化的**静态**用例（0.498，只过 check，不运行）：
# 多层联合 int | str | None 只去掉 None，剩下 int | str；isinstance 分支
# 之后按窄化后的联合检查。
#
# 注意：这里刻意不跑运行时 —— 联合 + 运行期 isinstance 判定在 master 上有
# 既有 bug（`isinstance(unknown_int, int)` 为 False，会落到错分支），不在本次
# 范围，已单独记录。

def multi(x: int | str | None) -> int:
    if x is None:
        return 0
    if isinstance(x, int):
        return x + 1
    return len(x)

def guard(b: bytes | None) -> int:
    if b is None:
        return -1
    return len(b)