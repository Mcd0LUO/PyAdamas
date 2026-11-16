# C 运行时抛出的异常（pa_raise_c 路径：IndexError / ZeroDivisionError / KeyError）
# 也必须能被零开销 try 抓到 —— 这些异常不是生成代码里的 raise，而是从 C 运行时
# 深处抛出来的，收口在 exc_longjmp：它按 pa_eh_depth 决定走 _Unwind 还是 longjmp。
#
# 形状必须满足 Z2 门禁：非可挂起、单层 try、catch-all、try 体里没有 return。
def idx_err() -> str:
    xs: list[int] = [1, 2]
    r: str = "none"
    try:
        n: int = xs[5]
        r = "got:" + str(n)
    except Exception as e:
        r = "caught:" + e
    return r


def div_err() -> str:
    r: str = "none"
    try:
        n: int = 1 // 0
        r = "got:" + str(n)
    except Exception as e:
        r = "caught:" + e
    return r


def key_err() -> str:
    d: dict[str, int] = {"a": 1}
    r: str = "none"
    try:
        n: int = d["nope"]
        r = "got:" + str(n)
    except Exception as e:
        r = "caught:" + e
    return r


print(idx_err())
print(div_err())
print(key_err())

# --- expect ---
# caught:list index out of range
# caught:integer division or modulo by zero
# caught:key not found
