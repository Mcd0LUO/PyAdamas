# 跳转线程化回归（0.335，PYADAMAS_LOOP_OPT bit0）：短路 and/or 链 + continue/break
# 在循环里的中转块被线程化后语义不变。nqueens 形态（早退路径占多数）。
def place(row: int, n: int, cols: list, d1: list, d2: list) -> int:
    if row == n:
        return 1
    total = 0
    for c in range(n):
        if cols[c] == 0 and d1[row + c] == 0 and d2[row - c + n] == 0:
            cols[c] = 1
            d1[row + c] = 1
            d2[row - c + n] = 1
            total += place(row + 1, n, cols, d1, d2)
            cols[c] = 0
            d1[row + c] = 0
            d2[row - c + n] = 0
    return total

def count(n: int) -> int:
    cols = []
    d1 = []
    d2 = []
    for _ in range(n):
        cols.append(0)
    for _ in range(2 * n):
        d1.append(0)
        d2.append(0)
    return place(0, n, cols, d1, d2)

print(count(8))

# continue/break + or 链
def f(n: int) -> int:
    t = 0
    for i in range(n):
        if i % 2 == 0 or i % 3 == 0:
            continue
        if i > 20:
            break
        t += i
    return t
print(f(30))
print(f(0))

# while + 嵌套 if 的中转块
def g(n: int) -> int:
    i = 0
    t = 0
    while i < n:
        if i % 5 == 0:
            t += 1
        else:
            if i % 7 == 0:
                t += 2
        i += 1
    return t
print(g(50))

# --- expect ---
# 92
# 73
# 0
# 22
