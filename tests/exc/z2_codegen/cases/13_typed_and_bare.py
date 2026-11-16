# Z3：typed catch + 裸 except 混用，验证分派链在遇到裸 except 后
# 不再往下判（后续 handler 成为死代码，与老路径一致）。
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
    except:
        r = "BARE"
    return r


print(f(1))
print(f(2))
print(f(3))

# --- expect ---
# VE
# BARE
# ok
