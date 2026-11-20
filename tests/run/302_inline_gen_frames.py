# 生成器帧不可内联钉子（PYADAMAS_INLINE_OPT）：生成器 callee 与生成器
# caller 一律保持独立帧；含 yield 的函数被高频调用、yield 在 try/finally
# 里、生成器提前关闭（close）语义都必须与 CPython 一致。
def gen(n: int) -> int:
    i: int = 0
    while i < n:
        yield i * i
        i += 1


def consume(n: int) -> int:
    total: int = 0
    for v in gen(n):
        total += v
    return total


s: int = 0
for r in range(300):
    s += consume(r % 7)
print(s)


def g2(n: int) -> int:
    try:
        yield n
        yield n + 1
    finally:
        pass


t: int = 0
for r in range(100):
    for v in g2(r):
        t += v
print(t)


def g3() -> int:
    yield 1
    yield 2
    yield 3


closed: int = 0
for r in range(50):
    g = g3()
    closed += next(g)
    g.close()
print(closed)

# --- expect ---
# 4460
# 10000
# 50
