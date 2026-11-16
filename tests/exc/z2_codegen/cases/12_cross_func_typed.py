# Z3：异常跨函数边界 + typed catch。调用方 try 里的 except 不匹配被调方
# 抛出的类型时，必须继续往外找 —— 由 pa_eh_rethrow 重启 phase 1 保证。
def inner(n: int) -> str:
    r: str = "none"
    try:
        if n == 1:
            raise ValueError("v")
        if n == 2:
            raise KeyError("k")
        r = "inner-ok"
    except KeyError:
        r = "inner-KE"
    return r


def outer(n: int) -> str:
    r: str = "none"
    try:
        r = inner(n)
    except ValueError:
        r = "outer-VE"
    return r


print(outer(1))
print(outer(2))
print(outer(3))

# --- expect ---
# outer-VE
# inner-KE
# inner-ok
