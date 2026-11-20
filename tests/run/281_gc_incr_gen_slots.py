# 增量标记正确性：生成器帧槽写（挂起间 churn 逼出 major，帧槽持活列表）。
def gen(n: int):
    keep: list = []
    i: int = 0
    while i < n:
        keep.append("item" + str(i))
        yield keep[i]
        i += 1

g = gen(30000)
total: int = 0
j: int = 0
for s in g:
    total += len(s)
    j += 1
    if j % 1000 == 0:
        k: int = 0
        while k < 2000:
            junk = ["churn" + str(k), k]
            k += 1
print(total)
# --- expect ---
# 258890
