# 434: complex 作返回/参数/变量标注不能被静默降级成 unknown。
# 之前 parse_annotation 没有 complex 分支，-> complex 退化成 unknown，
# 调用方按整数打印指针（94718388971600 这种）。


def pick(flag: bool) -> complex:
    if flag:
        return 1j
    return 2 + 3j


def scale(z: complex, k: float) -> complex:
    return z * k


def mag2(z: complex) -> float:
    a: float = abs(z)
    return a * a


z: complex = 3 + 4j
print(pick(True))
print(pick(False))
print(scale(z, 2.0))
print(mag2(z))
print(isinstance(z, complex))
# --- expect ---
# 1j
# (2+3j)
# (6+8j)
# 25.0
# True
