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
# 250000
# 60000
# 2500
# 3000
# 2 3
