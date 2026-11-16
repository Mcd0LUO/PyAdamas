# Z4：try-else。try 体正常结束后才执行 orelse，异常路径不执行。
# else 块在保护区之外（异常不可能跳到它），跑完再进 fin/end。
def f(n: int) -> str:
    r: str = "none"
    try:
        if n == 1:
            raise ValueError("v")
        r = "body"
    except ValueError:
        r = "caught"
    else:
        r = r + "+else"
    return r


def g(n: int) -> str:
    r: str = "none"
    try:
        if n == 1:
            raise ValueError("v")
        r = "body"
    except ValueError:
        r = "caught"
    else:
        r = r + "+else"
    finally:
        r = r + "+fin"
    return r


print(f(1))
print(f(2))
print(g(1))
print(g(2))

# --- expect ---
# caught
# body+else
# caught+fin
# body+else+fin
