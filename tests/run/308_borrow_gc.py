# 只读借用槽细化（0.338）GC 压力：借用元素经容器根可达，分配churn/
# 环垃圾触发 GC 后借用值仍存活；形参根由调用方帧持有。

def churn_borrow(xs: list[str], n: int) -> int:
    total = 0
    for i in range(n):
        s = xs[0]
        total += len(s)
        junk = ["junk" + str(i), i]
    return total

def cycle_churn(xs: list[str], n: int) -> int:
    total = 0
    for i in range(n):
        s = xs[0]
        total += len(s)
        a = []
        a.append(a)
    return total

def dict_churn(xs: list[str], n: int) -> int:
    total = 0
    for i in range(n):
        s = xs[i % len(xs)]
        d = {s: i}
        total += len(s) + d[s]
    return total

big = ["root" + str(i) for i in range(50)]
print(churn_borrow(big, 30000))
print(cycle_churn(big, 20000))
print(dict_churn(big, 20000))
print(len(big), big[0], big[49])

# --- expect ---
# 150000
# 100000
# 200106000
# 50 root0 root49
