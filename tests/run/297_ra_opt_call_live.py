# RA_OPT P1 回归（0.336，PYADAMAS_RA_OPT）：同块跨调用活跃值提升 callee-saved。
# cross 的 t=add1(a) 跨 mul2(b) 调用存活 —— P1 候选（非递归，提升纯赚）。
# fib 自递归：guard 整函数跳过提升（叶子调用白付 push/pop 税，实测负收益，
# 见 docs/ra-opt-design.md）。flag-on/off 输出都必须对。
def add1(x: int) -> int:
    return x + 1


def mul2(x: int) -> int:
    return x * 2


def cross(a: int, b: int) -> int:
    t = add1(a)
    u = mul2(b)
    return t + u


def fib(n: int) -> int:
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


s = 0
for i in range(20):
    s += cross(i, i * 3)
print(s)
print(fib(20))
# --- expect ---
# 1350
# 6765
