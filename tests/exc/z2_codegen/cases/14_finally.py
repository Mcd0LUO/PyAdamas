# Z4：finally。正常路径在保护区外跑一遍 finalbody；异常路径在 landing pad
# 里先跑 finalbody 再重抛。两条路径各内联一份，不会重复执行。
def normal() -> str:
    log: str = ""
    try:
        log = log + "T"
    finally:
        log = log + "F"
    return log


def caught() -> str:
    log: str = ""
    try:
        raise ValueError("v")
    except ValueError:
        log = log + "C"
    finally:
        log = log + "F"
    return log


print(normal())
print(caught())

# --- expect ---
# TF
# CF
