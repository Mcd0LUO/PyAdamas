# pyadamas: vect
# 461: P3 AVX2 4-lane 的边界/守卫回归（整数路径）。
# 4-lane 循环每次读 4 个相邻 i64（32 字节装载）、写 4 个；守卫只在前奏里做
# 一次（coef*(n-1)+inv+off < len），所以「最后一组 4 lane 正好落在数组末尾」
# 必须不越界。这里用**恰好长度**的数组把 4-lane 循环逼到边界，再用少一个元素
# 的数组验证越界仍然按标量时机抛 IndexError（不能先读后写）。
# 另外覆盖：同对象别名（同偏移才向量化，不同偏移保守标量）、负基址走标量、
# 浮点路径不受影响。


def mk(n: int) -> list[int]:
    a: list[int] = []
    for i in range(n):
        a.append((i * 5 + 1) % 7 - 3)
    return a


def zero(n: int) -> list[int]:
    a: list[int] = []
    for i in range(n):
        a.append(0)
    return a


# 恰好长度的 int map：out[j] = a[j]*2+1，n 从 4 到 9（每个 n 至少一次 4-lane）
def imap_exact(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = a[j] * 2 + 1


# 恰好长度的 int reduction
def idot_exact(a: list[int], b: list[int], n: int) -> int:
    s = 0
    for j in range(n):
        s += a[j] * b[j]
    return s


# 原地 map（同对象、同偏移）：只在 inv 运行期相等时才向量化
def imul_inplace(a: list[int], n: int) -> None:
    for j in range(n):
        a[j] = a[j] * a[j] - 1


# 同对象不同偏移：跨迭代有依赖，必须保守走标量
def ishift(a: list[int], n: int) -> None:
    for j in range(n):
        a[j] = a[j + 1] + 1


# 跨步装载（coef 2/3）+ 恰好长度
def istride(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = a[2 * j] * 3 - a[3 * j + 2]


def fmap(a: list[float], out: list[float], n: int) -> None:
    for j in range(n):
        out[j] = a[j] * 2.0 + 1.0


def mkf(n: int) -> list[float]:
    a: list[float] = []
    for i in range(n):
        a.append(float(i) * 0.25)
    return a


# ── 恰好长度：4-lane 循环跑到数组末尾 ──
for n in (4, 5, 6, 7, 8, 9):
    a = mk(n)
    out = zero(n)
    imap_exact(a, out, n)
    print("exact", n, out, idot_exact(a, a, n))

# n 小于 4：4-lane 不启动，2-lane/标量接手，输出必须一致
for n in (0, 1, 2, 3):
    a = mk(n)
    out = zero(n)
    imap_exact(a, out, n)
    print("small", n, out, idot_exact(a, a, n))

# ── 越界：数组比 n 少一个元素。守卫必须失败（宁可标量），IndexError 时机不变，
#    抛异常之前已写入的前缀与标量路径一致 ──
for n in (4, 5, 8):
    a = mk(n - 1)
    out = zero(n)
    try:
        imap_exact(a, out, n)
        print("oob", n, "no error", out)
    except IndexError:
        print("oob", n, "IndexError", out)

# 跨步装载越界：coef=2 时最后一个 lane 的下标是 2*(n-1)，数组恰好短一个
for n in (4, 5, 6):
    a = mk(2 * n - 1)
    out = zero(n)
    try:
        istride(a, out, n)
        print("soob", n, "no error", out)
    except IndexError:
        print("soob", n, "IndexError", out)

# ── 别名：同对象原地 map（向量化）与同对象不同偏移（必须标量）──
for n in (4, 5, 8):
    a = mk(n)
    imul_inplace(a, n)
    print("inpl", n, a)

for n in (4, 5, 8):
    a = mk(n + 1)
    ishift(a, n)
    print("shift", n, a)

# 恰好长度 + 跨步（gather 的 4 lane 打包要读 off+3*coef 位置的元素）
for n in (4, 5, 6, 7):
    a = mk(3 * n + 4)
    out = zero(n)
    istride(a, out, n)
    print("strd", n, out)

# ── 浮点路径（SSE2 2-lane，不变）──
for n in (4, 5, 8):
    f = mkf(n)
    fo = [0.0] * n
    fmap(f, fo, n)
    print("fmap", n, fo)

# --- expect ---
# exact 4 [-3, 7, 3, -1] 15
# exact 5 [-3, 7, 3, -1, -5] 24
# exact 6 [-3, 7, 3, -1, -5, 5] 28
# exact 7 [-3, 7, 3, -1, -5, 5, 1] 28
# exact 8 [-3, 7, 3, -1, -5, 5, 1, -3] 32
# exact 9 [-3, 7, 3, -1, -5, 5, 1, -3, 7] 41
# small 0 [] 0
# small 1 [-3] 4
# small 2 [-3, 7] 13
# small 3 [-3, 7, 3] 14
# oob 4 IndexError [-3, 7, 3, 0]
# oob 5 IndexError [-3, 7, 3, -1, 0]
# oob 8 IndexError [-3, 7, 3, -1, -5, 5, 1, 0]
# soob 4 IndexError [-7, 1, 0, 0]
# soob 5 IndexError [-7, 1, -12, 0, 0]
# soob 6 IndexError [-7, 1, -12, 0, 0, 0]
# inpl 4 [3, 8, 0, 0]
# inpl 5 [3, 8, 0, 0, 8]
# inpl 8 [3, 8, 0, 0, 8, 3, -1, 3]
# shift 4 [4, 2, 0, -2, -3]
# shift 5 [4, 2, 0, -2, 3, 2]
# shift 8 [4, 2, 0, -2, 3, 1, -1, 4, 3]
# strd 4 [-7, 1, -12, 3]
# strd 5 [-7, 1, -12, 3, 11]
# strd 6 [-7, 1, -12, 3, 11, -2]
# strd 7 [-7, 1, -12, 3, 11, -2, 6]
# fmap 4 [1.0, 1.5, 2.0, 2.5]
# fmap 5 [1.0, 1.5, 2.0, 2.5, 3.0]
# fmap 8 [1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5]
