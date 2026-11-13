# nqueens —— N 皇后回溯计数（递归 / 列表）
N = 12


def place(row, n, cols, d1, d2):
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


def count(n):
    cols = []
    d1 = []
    d2 = []
    for _ in range(n):
        cols.append(0)
    for _ in range(2 * n):
        d1.append(0)
        d2.append(0)
    return place(0, n, cols, d1, d2)


print(count(N))
