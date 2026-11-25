# 默认参数支持常量表达式（0.547）：两侧都是字面量的二元运算在编译期折叠。


def scale(x: int = 2 ** 10) -> int:
    return x


def shift(x: int = 1 << 4, y: float = 1.5 * 2.0) -> float:
    return x + y


def label(tag: str = "a" + "b") -> str:
    return tag


def mixed(n: int = 7 % 3, m: int = 100 // 7) -> int:
    return n + m


print(scale(), scale(3))
print(shift(), label(), mixed())

# --- expect ---
# 1024 3
# 19.0 ab 15
