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
# --- expect ---
# 88
# 370
# 45
