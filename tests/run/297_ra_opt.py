# 寄存器分配优化综合：活值、只读别名、异常/生成器/vthread 交互
# 0.381 合并自 3 个文件：297_ra_opt_call_live.py, 298_ra_opt_readonly_alias.py, 299_ra_opt_exc_gen_vthread.py

# ── 原 297_ra_opt_call_live.py ──────────────────────────────────────

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

# ── 原 298_ra_opt_readonly_alias.py ──────────────────────────────────────

# RA_OPT P2 回归（0.336，PYADAMAS_RA_OPT）：只读槽 load 别名。
# ro 的 row/n 只读：LICM 提升的多份副本别名到槽寄存器（nqueens 形态）。
# rw 的 n 中途被重新赋值 —— 绝不能别名（必须读到新值 107）。
# depth 的只读参数跨递归调用存活（callee-saved 跨调用安全）。
def ro(row: int, n: int, xs: list[int]) -> int:
    total = 0
    for c in range(n):
        if xs[c] == 0:
            total += row + c
            total += row - c + n
    return total


def rw(n: int) -> int:
    t = 0
    for i in range(n):
        t += n
    n = n + 100
    for i in range(3):
        t += n
    return t


xs = []
for i in range(12):
    xs.append(i % 3)
print(ro(5, 12, xs))
print(rw(7))


def depth(r: int, n: int) -> int:
    if r == n:
        return r
    return depth(r + 1, n) + r


print(depth(0, 9))

# ── 原 299_ra_opt_exc_gen_vthread.py ──────────────────────────────────────

# RA_OPT 正确性命门回归（0.336）：异常展开 / 生成器挂起 / vthread 切换下
# callee-saved 保存恢复。has_handlers / gen_ 函数整函数跳过提升（现有门槛），
# 只读参数副本在普通函数里别名后跨调用/跨挂起必须值正确。
def gen(n: int):
    acc = 0
    for i in range(n):
        acc += i
        yield acc


for x in gen(6):
    print(x)


def risky(n: int) -> int:
    t = 0
    for i in range(n):
        try:
            if i == 3:
                raise ValueError("x")
            t += i
        except ValueError:
            t += 100
    return t


print(risky(6))


def holder(ch, row: int, n: int):
    s = 0
    for c in range(n):
        s += row + c
    ch.send(s)
    ch.recv()
    print("vt", s, row, n)


ch = make_chan(int, 0)
h = go holder(ch, 7, 5)
v = ch.recv()
ch.send(1)
join(h)
print("done", v)

# --- expect ---
# 1350
# 6765
# 88
# 370
# 45
# 0
# 1
# 3
# 6
# 10
# 15
# 112
# vt 45 7 5
# done 45
