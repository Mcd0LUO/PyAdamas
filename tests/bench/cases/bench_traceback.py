"""traceback 基准：抛/捕 + format_exc 帧格式化。"""
import traceback

def inner(n: int) -> int:
    if n < 0:
        raise ValueError("bad " + str(n))
    return n + 1

def outer(n: int) -> int:
    return inner(n)

total = 0
r = 0
while r < 400:
    try:
        outer(-1)
    except ValueError:
        total += len(traceback.format_exc())
    r += 1
print(total > 0)
