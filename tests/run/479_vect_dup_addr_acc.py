# 重复地址原地累加写：`acc[0] -= e` / `acc[0] += e` 的地址不随 j 变 → 两个 lane
# 写**同一个地址**，不是 scatter。正确语义是按 lane 顺序合并（t ±= e0; t ±= e1），
# 与标量逐位一致；本轮把它折叠成一次有序写（阶段 B5 阶段 2）。
#
# 覆盖：n=0/1/2/3/5、累加/累减、顺序敏感数据（不同合并顺序会得到不同结果）、
# 与 map 写混在同一循环、以及「同一地址被第三次访问 → 必须保守拒绝整段标量」
# 的反例（late_read）。
import math


def acc_sub(acc: list[float], a: list[float], out: list[float], n: int) -> None:
    for j in range(n):
        acc[0] -= a[j] * 2.0
        acc[1] += a[j] * a[j]
        out[j] = a[j] + 1.0


def acc_order(acc: list[float], a: list[float], out: list[float], n: int) -> None:
    for j in range(n):
        acc[0] -= a[j]
        out[j] = a[j] * 0.5


def late_read(acc: list[float], a: list[float], out: list[float], n: int) -> None:
    # acc[0] 在体内被读第三次（累加后作为 map 值）→ 不能合并，整段标量。
    for j in range(n):
        acc[0] += a[j]
        out[j] = acc[0] * 2.0


def run(n: int) -> None:
    a = []
    out = []
    for j in range(8):
        a.append(float(j) - 3.5)
        out.append(0.0)
    acc = [0.0, 0.0]
    acc_sub(acc, a, out, n)
    tail = out[n - 1] if n > 0 else 0.0
    print("sub", n, acc[0], acc[1], out[0], tail)
    acc2 = [0.0, 0.0]
    acc_order(acc2, a, out, n)
    tail2 = out[n - 1] if n > 0 else 0.0
    print("ord", n, acc2[0], out[0], tail2)
    acc3 = [0.0, 0.0]
    late_read(acc3, a, out, n)
    tail3 = out[n - 1] if n > 0 else 0.0
    print("late", n, acc3[0], out[0], tail3)


# 顺序敏感：先减 1e16 再减 1.0 会被舍入吃掉，反过来不会。
b = [1.0, 1e16, -1e16, 1.0, 1e16, -1e16, 1.0, 1e16]
out = [0.0] * 8
for n in (0, 1, 2, 3, 5, 8):
    run(n)

# 顺序敏感单测（把 1.0 / 1e16 / -1e16 的相消顺序固定成会产生差别的值）
for order in (0, 1, 2, 3, 4, 5):
    seq = []
    for j in range(6):
        v = [1e16, 1.0, -1e16, 1e16, -1e16, 1.0][(j + order) % 6]
        seq.append(v)
    acc = [0.0, 0.0]
    o2 = [0.0] * 6
    acc_order(acc, seq, o2, 6)
    print("seq", order, acc[0], o2[0], o2[5])

# --- expect ---
# sub 0 0.0 0.0 0.0 0.0
# ord 0 0.0 0.0 0.0
# late 0 0.0 0.0 0.0
# sub 1 7.0 12.25 -2.5 -2.5
# ord 1 3.5 -1.75 -1.75
# late 1 -3.5 -7.0 -7.0
# sub 2 12.0 18.5 -2.5 -1.5
# ord 2 6.0 -1.75 -1.25
# late 2 -6.0 -7.0 -12.0
# sub 3 15.0 20.75 -2.5 -0.5
# ord 3 7.5 -1.75 -0.75
# late 3 -7.5 -7.0 -15.0
# sub 5 15.0 21.25 -2.5 1.5
# ord 5 7.5 -1.75 0.25
# late 5 -7.5 -7.0 -15.0
# sub 8 0.0 42.0 -2.5 4.5
# ord 8 0.0 -1.75 1.75
# late 8 0.0 -7.0 0.0
# seq 0 -1.0 5000000000000000.0 0.5
# seq 1 0.0 0.5 5000000000000000.0
# seq 2 -1.0 -5000000000000000.0 0.5
# seq 3 0.0 5000000000000000.0 -5000000000000000.0
# seq 4 0.0 -5000000000000000.0 5000000000000000.0
# seq 5 0.0 0.5 -5000000000000000.0
