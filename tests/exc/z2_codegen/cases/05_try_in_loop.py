# try 在循环里：每次进入 try 都要取一次临时栈水位（RcMark）、正常退出
# 清理一次（RcCleanup）；异常路径由 landing pad 清理。循环里反复走两条路径。
def loop(n: int) -> int:
    i: int = 0
    hits: int = 0
    while i < n:
        try:
            if i % 2 == 1:
                raise ValueError("odd")
            print("even", i)
        except Exception:
            hits = hits + 1
        i = i + 1
    return hits

print(loop(5))

# --- expect ---
# even 0
# even 2
# even 4
# 2
