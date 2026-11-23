# 不变量步长跨步写循环（0.449）：while q < n: buf[q] = 0; q = q + p
# 覆盖 p=1/2/3、残留尾、len < n 的 IndexError 时机、以及清零后计数。

def clear_stride(buf: list, n: int, p: int) -> None:
    q = 0
    while q < n:
        buf[q] = 0
        q = q + p


def count_nonzero(buf: list, n: int) -> int:
    c = 0
    i = 0
    while i < n:
        if buf[i] != 0:
            c = c + 1
        i = i + 1
    return c


def run_case(p: int, n: int) -> None:
    buf = []
    for i in range(20):
        buf.append(i + 1)
    clear_stride(buf, n, p)
    print("case", p, n, count_nonzero(buf, 20), buf[0], buf[1], buf[19])


run_case(1, 20)
run_case(2, 20)
run_case(3, 20)
run_case(2, 7)
run_case(3, 1)
run_case(3, 2)
e = 0
errcase = []
for i in range(5):
    errcase.append(i + 1)
try:
    clear_stride(errcase, 8, 2)
except IndexError:
    e = 1
print("index_error", e, errcase)
big = []
for i in range(1000):
    big.append(1)
clear_stride(big, 1000, 7)
print("big", count_nonzero(big, 1000))

# --- expect ---
# case 1 20 0 0 0 0
# case 2 20 10 0 2 20
# case 3 20 13 0 2 20
# case 2 7 16 0 2 20
# case 3 1 19 0 2 20
# case 3 2 19 0 2 20
# index_error 1 [0, 2, 0, 4, 0]
# big 857
