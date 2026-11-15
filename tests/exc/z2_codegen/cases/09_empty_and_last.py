# try 体为空（pass）、以及 try 是函数最后一条语句：保护区可能只有一条跳转，
# 甚至 begin/end 标签相邻。LSDA 要照发，控制流不能被 landing pad 带偏。
def empty_try() -> None:
    try:
        pass
    except Exception:
        print("never")
    print("empty ok")


def last_try() -> None:
    try:
        raise ValueError("last")
    except Exception as e:
        print("caught", e)


def last_quiet(n: int) -> None:
    try:
        if n == 5:
            raise KeyError("k")
    except Exception:
        print("quiet caught")


empty_try()
last_try()
last_quiet(5)
last_quiet(1)

# --- expect ---
# empty ok
# caught last
# quiet caught
