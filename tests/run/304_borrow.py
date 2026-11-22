# 借用优化综合：循环、对抗形态、内联
# 0.381 合并自 3 个文件：304_borrow_loop.py, 305_borrow_adversarial.py, 309_borrow_inline.py

# ── 原 304_borrow_loop.py ──────────────────────────────────────

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

# ── 原 305_borrow_adversarial.py ──────────────────────────────────────

# 只读借用槽细化（0.338）对抗形态：容器中途修改/覆盖写/别名/逃逸/del/
# unknown —— 必须保守跳过或安全变换，flag-on/off 行为逐字节一致。

def mutate_append(xs: list[str], n: int) -> int:
    total = 0
    for i in range(n):
        s = xs[0]
        total += len(s)
        xs.append("x" + str(i))
    return total

def mutate_set(xs: list[str], n: int) -> int:
    total = 0
    for i in range(n):
        s = xs[i % len(xs)]
        total += len(s)
        xs[i % len(xs)] = s + "!"
    return total

def mutate_del(xs: list[str], n: int) -> int:
    total = 0
    for i in range(n):
        if len(xs) == 0:
            break
        s = xs[0]
        total += len(s)
        del xs[0]
    return total

def mutate_sort(xs: list[str], n: int) -> int:
    total = 0
    for i in range(n):
        s = xs[0]
        total += len(s)
        if i % 7 == 0:
            xs.sort()
    return total

def reassign(xs: list[str], n: int) -> int:
    total = 0
    for i in range(n):
        s = xs[0]
        total += len(s)
        if i == n // 2:
            xs = ["replacement", "longer"]
    return total

def alias_mutate(xs: list[str], n: int) -> int:
    ys = xs
    total = 0
    for i in range(n):
        s = xs[0]
        total += len(s)
        ys.append("z")
    return total

def overwrite_s(xs: list[str], n: int) -> int:
    total = 0
    s = "init"
    for i in range(n):
        s = xs[0]
        total += len(s)
        s = s + "!"
    return total

def del_s(xs: list[str], n: int) -> int:
    total = 0
    for i in range(n):
        s = xs[0]
        total += len(s)
        del s
    return total

def self_assign(xs: list[str], n: int) -> int:
    total = 0
    for i in range(n):
        s = xs[0]
        s = s
        total += len(s)
    return total

def helper(xs: list[str]) -> int:
    xs.append("h")
    return 1

def escape_user(xs: list[str], n: int) -> int:
    total = 0
    for i in range(n):
        s = xs[0]
        total += len(s) + helper(xs)
    return total

def fresh_escape(n: int) -> int:
    xs: list[str] = ["a", "bb"]
    ys = xs
    total = 0
    for i in range(n):
        s = xs[0]
        total += len(s)
        ys.append("c")
    return total

def fresh_mutate_via_elem(n: int) -> int:
    xs: list[list[str]] = [["a"], ["b", "c"]]
    total = 0
    for i in range(n):
        s = xs[i % 2]
        s.append("x")
        total += len(s)
    return total

def unknown_slot(xs, n: int) -> int:
    total = 0
    for i in range(n):
        s = xs[0]
        total += len(s)
    return total

def obj_ann(xs: list[str], n: int) -> int:
    total = 0
    for i in range(n):
        s: object = xs[0]
        total += len(s)
    return total

xs = ["alpha", "beta", "gamma"]
print(mutate_append(xs, 50))
print(mutate_set(["p", "q"], 40))
print(mutate_del(["one", "two", "three", "four"], 10))
print(mutate_sort(["c", "a", "b"], 30))
print(reassign(xs, 20))
print(alias_mutate(["k"], 25))
print(overwrite_s(xs, 30))
print(del_s(xs, 30))
print(self_assign(xs, 30))
print(escape_user(["e"], 15))
print(fresh_escape(20))
print(fresh_mutate_via_elem(60))
print(unknown_slot(["u1", "u2"], 20))
print(obj_ann(xs, 20))

# ── 原 309_borrow_inline.py ──────────────────────────────────────

# 只读借用槽细化（0.338）内联协同：小借用函数被 O2 内联进调用方循环，
# 槽跨多次内联执行复用（借用槽只清零不释放）；不同实参交替；递归保守跳过。

def small_borrow(xs: list[str]) -> int:
    s = xs[0]
    return len(s)

def call_loop(xs: list[str], n: int) -> int:
    total = 0
    for i in range(n):
        total += small_borrow(xs)
    return total

def multi_call(a: list[str], b: list[str], n: int) -> int:
    total = 0
    for i in range(n):
        if i % 2 == 0:
            total += small_borrow(a)
        else:
            total += small_borrow(b)
    return total

def rec_borrow(xs: list[str], n: int) -> int:
    if n == 0:
        return 0
    s = xs[0]
    return len(s) + rec_borrow(xs, n - 1)

def borrow_then_grow(xs: list[str], n: int) -> int:
    total = 0
    for i in range(n):
        total += small_borrow(xs)
        xs.append("g" + str(i))
    return total

a = ["alpha", "beta"]
b = ["x", "yy", "zzz"]
print(call_loop(a, 50000))
print(multi_call(a, b, 20000))
print(rec_borrow(a, 500))
print(borrow_then_grow(["z"], 3000))
print(len(a), len(b))

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
# 250
# 420
# 15
# 30
# 154
# 25
# 150
# 150
# 150
# 30
# 20
# 1020
# 40
# 100
# 250000
# 60000
# 2500
# 3000
# 2 3
