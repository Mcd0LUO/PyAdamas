# 419: 块内标注声明与函数级名字同类型时照旧（0.361 收紧的边界）。
# 收紧的是「同一函数作用域内一个名字一种标注类型」，块内声明也算；
# 但同名同类型、嵌套函数、类体、模块级、for 变量这些形态不受影响。
def same_type(flag: bool) -> int:
    c: int = 1
    if flag:
        c: int = 2
    return c


def nested() -> str:
    c: int = 1
    def inner() -> str:
        c: str = "x"
        return c
    print(c)
    return inner()


def class_body() -> int:
    c: int = 3
    class K:
        c: str = "y"
    return c + len(K().c)


def loop_var() -> int:
    total: int = 0
    for total in [1, 2]:
        pass
    return total


mod_level: str = "m"
def shadows_module() -> int:
    mod_level: int = 9
    return mod_level

print(same_type(True), same_type(False))
print(nested())
print(class_body())
print(loop_var())
print(shadows_module(), mod_level)
# --- expect ---
# 2 1
# 1
# x
# 4
# 2
# 9 m
