# 只读借用槽细化（0.338，PYADAMAS_BORROW_OPT）：命中形态。
# 循环变量从稳定容器借用元素、全程只读 → 免每轮 inc/dec 与出口 release。
# 计数守恒：容器持有元素那份引用贯穿函数体（见 lower_borrow.cpp 论证）。

def probe_range(xs: list[str], n: int) -> int:
    total = 0
    for i in range(n):
        s = xs[0]
        total += len(s)
    return total

def probe_each(xs: list[str], n: int) -> int:
    total = 0
    for i in range(n):
        s = xs[i % len(xs)]
        total += len(s)
    return total

def wordfreq(words: list[str]) -> dict[str, int]:
    d: dict[str, int] = {}
    for w in words:
        d[w] = d.get(w, 0) + 1
    return d

def while_borrow(xs: list[str], n: int) -> int:
    total = 0
    i = 0
    while i < n:
        s = xs[i % len(xs)]
        total += len(s)
        i += 1
    return total

def nested(rows: list[list[str]], n: int) -> int:
    total = 0
    for i in range(n):
        row = rows[i % len(rows)]
        for j in range(len(row)):
            s = row[j]
            total += len(s)
    return total

def tuple_root(t: tuple[str, str, str], n: int) -> int:
    total = 0
    for i in range(n):
        s = t[i % 3]
        total += len(s)
    return total

def fresh_local(n: int) -> int:
    xs: list[str] = ["alpha", "beta", "gamma"]
    total = 0
    state = 12345
    for i in range(n):
        state = (state * 1103515245 + 12345) % 2147483648
        s = xs[state % 3]
        total += len(s)
    return total

def fresh_methods(n: int) -> int:
    xs: list[str] = ["Alpha", "beta", "GAMMA"]
    total = 0
    for i in range(n):
        s = xs[i % 3]
        total += len(s.lower()) + len(s.upper())
    return total

def borrow_uses(xs: list[str], n: int) -> str:
    out: list[str] = []
    d: dict[str, int] = {}
    for i in range(n):
        s = xs[i % len(xs)]
        if len(s) > 3:
            d[s] = d.get(s, 0) + 1
            out.append(s + "!")
        t = s
        if t == xs[0]:
            out.append(t)
    return ",".join(out) + "|" + str(len(d))

def early_return(xs: list[str], n: int) -> str:
    for i in range(n):
        s = xs[i % len(xs)]
        if len(s) == 4:
            return s
    return "none"

xs = ["alpha", "beta", "gamma", "delta"]
print(probe_range(xs, 2000))
print(probe_each(xs, 2000))
print(wordfreq(["a", "b", "a", "c", "b", "a"]))
print(while_borrow(xs, 1000))
print(nested([["a", "bb"], ["ccc", "dddd"]], 500))
print(tuple_root(("x", "yy", "zzz"), 999))
print(fresh_local(2000))
print(fresh_methods(1000))
print(borrow_uses(xs, 12))
print(early_return(xs, 10))

# --- expect ---
# 10000
# 9500
# {'a': 3, 'b': 2, 'c': 1}
# 4750
# 2500
# 1998
# 9313
# 9334
# alpha!,alpha,beta!,gamma!,delta!,alpha!,alpha,beta!,gamma!,delta!,alpha!,alpha,beta!,gamma!,delta!|4
# beta
