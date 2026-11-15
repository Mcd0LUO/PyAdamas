# 裸 raise：正在处理里层 try 时，函数被调用方在 except 体里调用，
# 当前异常上下文来自调用方。新路径要走 pa_eh_reraise（_Unwind 版裸 raise），
# 不能退化成 longjmp。
def reraiser() -> None:
    try:
        raise
    except Exception as e:
        print("inner", e)

def outer() -> None:
    try:
        raise ValueError("boom")
    except Exception:
        reraiser()

outer()

# --- expect ---
# inner boom
