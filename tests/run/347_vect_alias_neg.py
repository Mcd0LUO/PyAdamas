# pyadamas: vect
# 通用向量化别名/依赖负例（PYADAMAS_VECT）：证不出安全 → 保守标量 fallback，
# 输出仍须正确（==CPython）。覆盖：同对象别名、跨迭代依赖、写后读、循环变量外读。
def alias(a: list[float], out: list[float], n: int) -> None:
    for i in range(n):
        out[i] = a[i] * 2.0

def dep_shift(a: list[float], n: int) -> None:
    for i in range(n):
        a[i] = a[i + 1] + 1.0   # 写 off0 读 off1 同数组 → 跨迭代依赖 → 跳过

def wr_then_rd(a: list[float], out: list[float], n: int) -> float:
    s = 0.0
    for i in range(n):
        out[i] = a[i] * 3.0
        s += out[i]             # out 写后读 → 跳过
    return s

def liveout(a: list[float], n: int) -> None:
    for i in range(n):
        a[i] = a[i] + 1.0
    print("last", i)            # 循环变量副本槽外部有读 → 跳过

def mk(n: int) -> list[float]:
    a = []
    for i in range(n):
        a.append(float(i))
    return a

# 别名：out 与 a 同一对象（守卫判等失败 → 标量）
a = mk(6)
alias(a, a, 6)
print("alias", a)
# 依赖：左移
b = mk(6)
dep_shift(b, 5)
print("dep", b)
# 写后读
c = mk(6)
o = mk(6)
print("wr", wr_then_rd(c, o, 6), o)
# 循环变量外读
d = mk(4)
liveout(d, 4)
print("liveout", d)

# --- expect ---
# alias [0.0, 2.0, 4.0, 6.0, 8.0, 10.0]
# dep [2.0, 3.0, 4.0, 5.0, 6.0, 5.0]
# wr 45.0 [0.0, 3.0, 6.0, 9.0, 12.0, 15.0]
# last 3
# liveout [1.0, 2.0, 3.0, 4.0]
