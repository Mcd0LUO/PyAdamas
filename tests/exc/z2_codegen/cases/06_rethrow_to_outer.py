# 新→新：内层 catch-all（except Exception）接不住 SystemExit，
# landing 分发不匹配 → pa_eh_rethrow 重新展开，外层 catch-all 接住。
# 同时验证跨函数的表驱动展开（内层帧由 personality 的批量帧清理释放）。
def inner() -> None:
    try:
        raise SystemExit("bye")
    except Exception:
        print("inner should not print")

def outer() -> None:
    try:
        inner()
    except:
        print("outer caught")

outer()

# --- expect ---
# outer caught
