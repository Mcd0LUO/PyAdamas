# `T | None` 窄化（0.498）：分支内/提前返回/真值/assert/短路/多层联合/赋值合并。
# expect 段来自 CPython 3.14.4 实跑（本文件只做静态检查，运行输出与窄化无关）。

def one(x: int | None) -> int:
    if x is not None:
        return x + 1
    return 0

def two(x: int | None) -> int:
    if x is None:
        return 0
    return x + 1

def three(x: str | None) -> int:
    if x is None:
        return -1
    else:
        return len(x)

def four(xs: list[int] | None) -> int:
    if xs:
        return xs[0] + len(xs)
    return 0

def five(x: int | None) -> int:
    assert x is not None
    return x * 2

class Box:
    v: int
    def __init__(self, v: int) -> None:
        self.v = v

def six(b: Box | None) -> int:
    if b is not None and b.v > 0:
        return b.v
    return 0

def eight(x: int | None) -> int:
    if x is None:
        x = 0
    return x + 1

def nine(x: int | None) -> int:
    if x is None:
        return 0
    y: int = x
    x = None
    return y + 1

print(one(1), one(None), two(2), two(None))
print(three("abc"), three(None))
print(four([1, 2]), four(None))
print(five(3), six(Box(4)), six(None))
print(eight(None), nine(9), nine(None))

# --- expect ---
# 2 0 3 0
# 3 -1
# 3 0
# 6 4 0
# 1 10 0
