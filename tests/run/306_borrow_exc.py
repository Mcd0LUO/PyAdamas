# 只读借用槽细化（0.338）异常路径：借用槽不进帧释放清单，展开由容器
# owner 负责；try/except/finally/早退/嵌套/抛出点夹心均须与 flag-off 一致。

def exc_index(xs: list[str], n: int) -> int:
    total = 0
    try:
        for i in range(n):
            s = xs[i]
            total += len(s)
    except IndexError:
        total += 1000
    return total

def exc_raise(xs: list[str], n: int) -> int:
    total = 0
    try:
        for i in range(n):
            s = xs[0]
            total += len(s)
            if i == 5:
                raise ValueError("mid")
    except ValueError:
        total += 100
    return total

def exc_finally(xs: list[str], n: int) -> int:
    total = 0
    d: dict[str, int] = {}
    try:
        for i in range(n):
            s = xs[i % len(xs)]
            d[s] = d.get(s, 0) + 1
            total += len(s)
            if i == 7:
                return total + 5
    finally:
        total += len(d)
    return total

def exc_nested(xs: list[str], n: int) -> int:
    total = 0
    for i in range(n):
        s = xs[i % len(xs)]
        try:
            if len(s) == 5:
                raise KeyError(s)
            total += len(s)
        except KeyError:
            total += 10
    return total

def exc_after_loop(xs: list[str], n: int) -> int:
    total = 0
    s = "none"
    try:
        for i in range(n):
            s = xs[i % len(xs)]
            total += len(s)
            if i == 9:
                raise RuntimeError("stop")
    except RuntimeError:
        total += len(s)
    return total

def exc_two_loops(xs: list[str], n: int) -> int:
    total = 0
    try:
        for i in range(n):
            s = xs[0]
            total += len(s)
        for i in range(n):
            t = xs[i % len(xs)]
            total += len(t)
            if i == 6:
                raise ValueError("second")
    except ValueError:
        total += 7
    return total

xs = ["alpha", "beta", "gamma", "delta"]
print(exc_index(xs, 10))
print(exc_raise(xs, 20))
print(exc_finally(xs, 20))
print(exc_nested(xs, 24))
print(exc_after_loop(xs, 20))
print(exc_two_loops(xs, 12))

# --- expect ---
# 1019
# 130
# 43
# 204
# 51
# 100
