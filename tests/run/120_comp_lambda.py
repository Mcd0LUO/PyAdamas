# 推导式里的 lambda：含 lambda 的 list/set/dict 推导式脱糖成
#「生成器表达式 + 构造器」，循环变量成为合成函数的局部变量，闭包捕获
# 与 CPython 一致（所有 lambda 共享循环结束时的值）。
def main() -> None:
    fs = [lambda: i * 10 for i in range(3)]
    for f in fs:
        print(f())
    gs = [lambda: i + j for i in range(2) for j in range(2)]
    for g in gs:
        print(g())
    hs = [x for x in range(4) if (lambda: x % 2 == 0)()]
    print(hs)
    ks = [lambda v: v + i for i in range(3)]
    for k in ks:
        print(k(100))
    ds = {i: (lambda: i * 2) for i in range(2)}
    print(ds[0](), ds[1]())
    pairs = dict((x, x * x) for x in range(3))
    print(pairs[0], pairs[1], pairs[2])
    q = dict([(1, 2), (3, 4)])
    print(q[1], q[3])
    ns = [[lambda: i for j in range(1)] for i in range(2)]
    print(ns[0][0](), ns[1][0]())


main()

# --- expect ---
# 20
# 20
# 20
# 2
# 2
# 2
# 2
# [0, 2]
# 102
# 102
# 102
# 2 2
# 0 1 4
# 2 4
# 1 1
