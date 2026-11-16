# Z4：嵌套 try。同一函数的多个保护区合成一份 LSDA（多条 call-site 记录），
# 共用同一个 landing pad 分发入口。内层 handler 不匹配时 pa_eh_rethrow 重启
# phase 1，外层 handler 仍能接住。
def f(n: int) -> str:
    r: str = "none"
    try:
        try:
            if n == 1:
                raise ValueError("v")
            if n == 2:
                raise KeyError("k")
            r = "inner-ok"
        except KeyError:
            r = "inner-KE"
    except ValueError:
        r = "outer-VE"
    return r


print(f(1))
print(f(2))
print(f(3))

# --- expect ---
# outer-VE
# inner-KE
# inner-ok
