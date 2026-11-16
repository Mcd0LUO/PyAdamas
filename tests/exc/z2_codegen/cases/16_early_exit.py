# Z4：早退穿过保护区。return/break/continue 提前离开 try 体时，
# emit_cleanup_exits 会先 pa_eh_leave（退出表驱动展开区）再按水位清临时值。
# 没有这一步，pa_eh_depth 会一直 > 0，之后抛出的异常会误走 _Unwind。
def f(n: int) -> str:
    try:
        if n == 1:
            return "early"
        if n == 2:
            raise ValueError("v")
    except ValueError:
        return "caught"
    return "late"


def g() -> str:
    out: str = ""
    i: int = 0
    while i < 5:
        try:
            if i == 2:
                break
            out = out + str(i)
        except ValueError:
            out = out + "E"
        i += 1
    return out


print(f(1))
print(f(2))
print(f(3))
print(g())

# --- expect ---
# early
# caught
# late
# 01
