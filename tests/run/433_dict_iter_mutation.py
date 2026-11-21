# 433: 迭代中修改 dict/set 必须抛可捕获的 RuntimeError（CPython 语义）。
# 旧行为有两种错法：删空之后被下标上界悄悄结束（静默），
# 或者走到越界读时 panic（进程直接退出、except 接不住）。


def dict_del() -> str:
    d: dict[int, int] = {1: 1}
    try:
        for k in d:
            del d[k]
    except RuntimeError as e:
        return "RuntimeError: " + str(e)
    return "no error"


def set_add() -> str:
    s: set[int] = set()
    s.add(1)
    try:
        for x in s:
            s.add(2)
    except RuntimeError as e:
        return str(e)
    return "no error"


def dict_add() -> str:
    d: dict[int, int] = {1: 1, 2: 2}
    try:
        for k in d:
            d[99] = 9
    except RuntimeError as e:
        return "RuntimeError: " + str(e)
    return "no error"


def enum_mut() -> str:
    d: dict[str, int] = {"a": 1}
    try:
        for i, k in enumerate(d):
            d["b"] = 2
    except RuntimeError as e:
        return "RuntimeError: " + str(e)
    return "no error"


def plain() -> str:
    d: dict[int, int] = {1: 10, 2: 20}
    out: str = ""
    for k in d:
        out = out + str(k) + "=" + str(d[k]) + " "
    s: set[int] = set()
    s.add(7)
    s.add(8)
    n: int = 0
    for v in s:
        n = n + v
    return out + "/" + str(n)


print(dict_del())
print(set_add())
print(dict_add())
print(enum_mut())
print(plain())
# --- expect ---
# RuntimeError: dictionary changed size during iteration
# Set changed size during iteration
# RuntimeError: dictionary changed size during iteration
# RuntimeError: dictionary changed size during iteration
# 1=10 2=20 /15
