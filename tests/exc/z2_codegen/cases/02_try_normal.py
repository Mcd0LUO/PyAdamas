# try 体正常完成（不抛异常）：新路径在非异常路径上不能改变任何行为。
def count(n: int) -> int:
    total: int = n
    try:
        total = total + 1
        total = total * 2
    except Exception:
        total = -1
    return total

print(count(20))
print(count(0))

# --- expect ---
# 42
# 2
