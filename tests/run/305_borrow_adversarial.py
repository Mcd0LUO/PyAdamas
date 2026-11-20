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

# --- expect ---
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
