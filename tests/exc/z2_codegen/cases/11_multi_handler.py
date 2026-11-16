# Z3：多 handler 分派链。异常按 handler 顺序逐个 lower_exc_match，
# 都不匹配时走 pa_eh_rethrow 重启 phase 1（与老路径 ExcReraise 等价）。
def f(n: int) -> str:
    r: str = "none"
    try:
        if n == 1:
            raise ValueError("v")
        if n == 2:
            raise KeyError("k")
        r = "ok"
    except ValueError:
        r = "VE"
    except KeyError:
        r = "KE"
    except Exception:
        r = "EX"
    return r


print(f(1))
print(f(2))
print(f(3))

# --- expect ---
# VE
# KE
# ok
