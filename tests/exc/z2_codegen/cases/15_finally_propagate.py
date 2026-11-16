# Z4：try/finally 没有 except —— 异常必须穿过 finally 继续向上抛，
# 由调用方的 except 接住。finally 体要在异常传播前执行。
def f() -> str:
    log: str = ""
    try:
        raise ValueError("v")
    finally:
        log = log + "F"
    return log


def g() -> str:
    r: str = "none"
    try:
        r = f()
    except ValueError:
        r = "caught"
    return r


print(g())

# --- expect ---
# caught
