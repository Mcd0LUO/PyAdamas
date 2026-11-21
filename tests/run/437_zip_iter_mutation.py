# 437: zip 迭代中改 dict/set 也要抛可捕获的 RuntimeError（与 for k in d 同口径）。
# zip 的长度是入口算的 min(len a, len b)，删短其中一个会让两者相对关系变化，
# 所以两个操作数各存一份入口长度逐轮比对。


def del_first() -> str:
    d: dict[str, int] = {"a": 1, "b": 2}
    e: dict[str, int] = {"x": 1, "y": 2}
    try:
        for k, v in zip(d, e):
            del d[k]
    except RuntimeError as err:
        return "RuntimeError: " + str(err)
    return "no error"


def del_second() -> str:
    a: dict[str, int] = {"x": 1, "y": 2}
    b: dict[str, int] = {"x": 3, "y": 4}
    try:
        for k, v in zip(a, b):
            del b[k]
            print("never")
    except RuntimeError as err:
        return "RuntimeError: " + str(err)
    return "no error"


def set_mutate() -> str:
    s: set[int] = set()
    s.add(1)
    t: set[int] = set()
    t.add(2)
    try:
        for x, y in zip(s, t):
            s.add(3)
    except RuntimeError as err:
        return str(err)
    return "no error"


def plain() -> str:
    a: dict[str, int] = {"a": 1, "b": 2}
    b: dict[str, int] = {"x": 1, "y": 2}
    out: str = ""
    for k, v in zip(a, b):
        out = out + k + str(v)
    l1: list[int] = [1, 2, 3]
    l2: list[int] = [4, 5]
    n: int = 0
    for x, y in zip(l1, l2):
        n = n + x * y
    return out + "/" + str(n)


print(del_first())
print(del_second())
print(set_mutate())
print(plain())
# --- expect ---
# RuntimeError: dictionary changed size during iteration
# never
# RuntimeError: dictionary changed size during iteration
# Set changed size during iteration
# axby/14
