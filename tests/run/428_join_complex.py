# join(int/float, complex) 应该是 complex（CPython 数值塔）。
# 原来落到 t_unknown()：三元表达式把复数塞进 8 字节 unknown 槽，打印时
# 按整数读复数的堆指针（输出 9xxxxx 地址）；else 分支的 int/float 又没转
# 成复数，分支一旦走到 int 侧就把 1 当复数指针解引用、直接段错误。

def pick(c: bool) -> None:
    x = 1 if c else 1j
    print(x)

def pick2(c: bool) -> None:
    y = 2.5 if c else 3j
    print(y)

def via_if(c: bool) -> None:
    v = 1
    if c:
        v = 1j
    print(v)

pick(True)
pick(False)
pick2(True)
pick2(False)
via_if(True)
print(1 + 1j)
# --- expect ---
# (1+0j)
# 1j
# (2.5+0j)
# 3j
# 1j
# (1+1j)
