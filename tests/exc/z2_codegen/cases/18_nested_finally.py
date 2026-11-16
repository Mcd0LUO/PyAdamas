# Z4：嵌套 + finally。内层 try/finally 抛出的异常穿过 finally 后由外层
# try/except 接住；finally 体在异常传播前执行且只执行一次。
def inner() -> str:
    log: str = ""
    try:
        raise ValueError("v")
    finally:
        log = log + "F"
    return log


def outer() -> str:
    r: str = "none"
    try:
        r = inner()
    except ValueError:
        r = "caught"
    return r


print(outer())

# --- expect ---
# caught
