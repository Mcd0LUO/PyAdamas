# join(int, bool) 应该是 int，不是 float：bool 是 int 的子类型。
# 原来 Bool 不进整数提升分支，落到 t_float()：x * 2 打出 2.0，
# 并且 return 7 if c else True 在 -> int 的函数里直接编译失败
# （declared to return 'int', got 'float'）。

def pick(c: bool) -> int:
    return 7 if c else True

def show(c: bool) -> None:
    x = 1 if c else True
    print(x * 2, x + 1)

def mixed(c: bool) -> None:
    y = True if c else 2
    print(y * 2)

print(pick(True), pick(False))
show(True)
show(False)
mixed(True)
mixed(False)
# --- expect ---
# 7 1
# 2 2
# 2 2
# 2
# 4
