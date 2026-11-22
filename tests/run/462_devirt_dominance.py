# 462: devirt 的支配性判据（0.390）。
# 槽里存函数值时，只有「唯一那条 store.slot **支配**调用点」才能证明被调唯一。
# 形参槽在入口还有一份调用方给的初值（不在 IR 里）：store 只在某个分支里时，
# 调用点完全可能读到那个初值 —— 这种形态必须放过，否则会编译成错的直调。


def dbl(x: int) -> int:
    return x * 2


def plus1(x: int) -> int:
    return x + 1


def apply_cond(use_dbl: int, x: int) -> int:
    # 槽的 store 只在分支里 → 不支配调用点 → 不得去虚拟化
    f = plus1
    if use_dbl > 0:
        f = dbl
    return f(x)


def apply_param(f: object, use_dbl: int, x: int) -> int:
    # 形参槽：入口有调用方给的初值，同样不得去虚拟化
    if use_dbl > 0:
        f = dbl
    return f(x)


print(apply_cond(1, 10))
print(apply_cond(0, 10))
print(apply_param(plus1, 1, 10))
print(apply_param(plus1, 0, 10))

# 对照组：全局只写一次 + 直调，应该正常（输出与上面无关）
g = dbl
print(g(21))
# --- expect ---
# 20
# 11
# 20
# 11
# 42
