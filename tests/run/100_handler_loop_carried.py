# longjmp 会把 callee-saved 寄存器恢复到 setjmp 时刻。循环体内 try 里写的
# 变量，如果循环头还要读，就不能提升到寄存器——否则回边那次迭代读到的是
# 回退前的旧值（静默算错，不报错）。
def loop_carried() -> int:
    x: int = 0
    i: int = 0
    out: int = 0
    while i < 3:
        out = out * 10 + x
        try:
            x = i + 1
            if i == 1:
                raise ValueError("boom")
        except ValueError:
            pass
        i = i + 1
    return out


# 保护区内的写 + 保护区外读（正向块序）
def write_in_try() -> int:
    v: int = 0
    try:
        v = 42
        raise KeyError("k")
    except KeyError:
        pass
    return v


# 保护区内的写 + except 里读
def read_in_except() -> int:
    x: int = 0
    try:
        x = 777
        raise ValueError("boom")
    except ValueError:
        return x + 1
    return -1


print(loop_carried())
print(write_in_try())
print(read_in_except())

# --- expect ---
# 12
# 42
# 778
