# 0.565：PEP 758 —— except 可以不带括号写多个异常（3.14 语法）。
def f(kind: int) -> str:
    try:
        if kind == 1:
            raise ValueError("v")
        raise KeyError("k")
    except ValueError, KeyError:
        return "caught"
    return "none"

print(f(1), f(2))

# --- expect ---
# caught caught
