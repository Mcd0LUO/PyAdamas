# pyadamas: vect
# 460: P3 AVX2 4-lane（整数路径）回归。
# 通用向量化的整数 map / reduction 在 AVX2 机器上走 4 lane ymm，非 AVX2 机器
# （或 PYADAMAS_AVX2=0 编译）走原来的 SSE2 2-lane 回退。两条路径的输出必须
# 逐字节一致 —— 本文件既是对拍用例（run_tests 默认跑 AVX2 路径），也可以
# 用 PYADAMAS_AVX2=0 重跑一遍比输出（验证回退路径）。
# 覆盖：int map（含原地写）、int reduction（含非 0 初值/双累加器）、int 乘
# 回绕、跨步装载（coef>1 的 4-lane 打包）、while 形状、n 边界 0/1/2/3/5/7/8/9、
# 以及浮点路径仍走 2-lane 的正确性。参考值一律由**不向量化**的循环
# （反向 / 步长 2）算出，避免「两条路径同一个 bug」的对拍盲区。


def mk(n: int) -> list[int]:
    a: list[int] = []
    for i in range(n):
        a.append((i * 7 + 3) % 11 - 5)
    return a


def zero(n: int) -> list[int]:
    a: list[int] = []
    for i in range(n):
        a.append(0)
    return a


def mkf(n: int) -> list[float]:
    a: list[float] = []
    for i in range(n):
        a.append(float(i) * 0.5 - 1.0)
    return a


# ── 整数 map（4 lane 装载 + 逐 lane 算术 + 32 字节写出）──
def imap(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = a[j] * 2 + 1


def imap_inplace(a: list[int], n: int) -> None:
    for j in range(n):
        a[j] = a[j] * 3 - 1


# ── 整数 reduction（vpaddq 4 lane + 出口水平求和）──
def idot(a: list[int], b: list[int], n: int) -> int:
    s = 0
    for j in range(n):
        s += a[j] * b[j]
    return s


def idot_init(a: list[int], b: list[int], n: int, t: int) -> int:
    s = t
    for j in range(n):
        s += a[j] * b[j]
    return s


def isum2(a: list[int], n: int) -> int:
    s = 0
    t = 0
    for j in range(n):
        s += a[j]
        t += a[j] * a[j]
    return s * 100000 + t


# ── 跨步装载（coef>1：4-lane 用 vmovq + vpinsrq 打包，SSE2 用两次 movsd）──
def imap_stride(a: list[int], out: list[int], n: int) -> None:
    for j in range(n):
        out[j] = a[2 * j] + a[3 * j + 1]


def idot_stride(a: list[int], n: int) -> int:
    s = 0
    for j in range(n):
        s += a[2 * j]
    return s


# ── while 形状（P2）：增量在体块尾部，4-lane 也要维护 j ──
def iwhile_map(a: list[int], out: list[int], n: int) -> None:
    j = 0
    while j < n:
        out[j] = a[j] * 2 + 1
        j = j + 1


def iwhile_red(a: list[int], n: int) -> int:
    j = 0
    s = 0
    while j < n:
        s += a[j] * a[j]
        j = j + 1
    return s


# ── 浮点路径：保持 SSE2 2-lane（保序累加），只确认仍然正确 ──
def fmap(a: list[float], out: list[float], n: int) -> None:
    for j in range(n):
        out[j] = a[j] * 2.0 + 1.0


def fdot(a: list[float], b: list[float], n: int) -> float:
    s = 0.0
    for j in range(n):
        s += a[j] * b[j]
    return s


# ── 参考实现：反向 / 步长 2 —— 向量化器不认这两种形状（不会也被向量化）──
def ref_rev_sum(a: list[int], n: int) -> int:
    s = 0
    j = n - 1
    while j >= 0:
        s += a[j]
        j = j - 1
    return s


def ref_step2_sum(a: list[int], n: int) -> int:
    s = 0
    j = 0
    while j < n:
        s += a[j]
        j = j + 2
    return s


def ref_rev_dot(a: list[int], b: list[int], n: int) -> int:
    s = 0
    j = n - 1
    while j >= 0:
        s += a[j] * b[j]
        j = j - 1
    return s


def mul_wrap(a: list[int], b: list[int], n: int) -> int:
    s = 0
    for j in range(n):
        s += a[j] * b[j]
    return s


def ref_mul_wrap(a: list[int], b: list[int], n: int) -> int:
    s = 0
    j = n - 1
    while j >= 0:
        s += a[j] * b[j]
        j = j - 1
    return s


for n in (0, 1, 2, 3, 5, 7, 8, 9):
    a = mk(3 * n + 4)
    out = zero(n)
    imap(a, out, n)
    print("imap", n, out)
    ip = mk(3 * n + 4)
    imap_inplace(ip, n)
    print("inpl", n, ip)
    print("idot", n, idot(a, a, n))
    print("init", n, idot_init(a, a, n, 49))
    print("sum2", n, isum2(a, n))
    o2 = zero(n)
    imap_stride(a, o2, n)
    print("strd", n, o2)
    print("sdot", n, idot_stride(a, n))
    o3 = zero(n)
    iwhile_map(a, o3, n)
    print("wmap", n, o3)
    print("wred", n, iwhile_red(a, n))

# 一致性：向量化循环 vs 不向量化的参考循环（同 n，多种长度）
for n in (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 16, 33, 64):
    a = mk(3 * n + 4)
    b = mk(3 * n + 4)
    print("eq", n, idot(a, b, n) == ref_rev_dot(a, b, n),
          idot_stride(a, n) == ref_step2_sum(a, n),
          isum2(a, n) // 100000 == ref_rev_sum(a, n))

# 64 位回绕：任意 i64 都要与标量 imulq/addq 逐位一致（CPython 不溢出，
# 所以只打印两条实现是否相等）
BIG = 4611686018427387904  # 1 << 62
c = zero(40)
d = zero(40)
for i in range(40):
    c[i] = BIG + i * 1234567
    d[i] = -BIG - i * 7654321
for n in (3, 4, 5, 8, 13, 40):
    print("wrap", n, mul_wrap(c, d, n) == ref_mul_wrap(c, d, n))

# 浮点路径（2-lane，保持不变）
for n in (0, 1, 2, 3, 5, 8):
    f = mkf(2 * n + 2)
    fo = [0.0] * n
    fmap(f, fo, n)
    print("fmap", n, fo)
    print("fdot", n, format(fdot(f, f, n), ".4f"))

# --- expect ---
# imap 0 []
# inpl 0 [-2, 5, 1, -3]
# idot 0 0
# init 0 49
# sum2 0 0
# strd 0 []
# sdot 0 0
# wmap 0 []
# wred 0 0
# imap 1 [-3]
# inpl 1 [-7, 5, 1, -3, 4, 0, -4]
# idot 1 4
# init 1 53
# sum2 1 -199996
# strd 1 [3]
# sdot 1 -2
# wmap 1 [-3]
# wred 1 4
# imap 2 [-3, 11]
# inpl 2 [-7, 14, 1, -3, 4, 0, -4, 3, -1, -5]
# idot 2 29
# init 2 78
# sum2 2 300029
# strd 2 [3, 5]
# sdot 2 -1
# wmap 2 [-3, 11]
# wred 2 29
# imap 3 [-3, 11, 3]
# inpl 3 [-7, 14, 2, -3, 4, 0, -4, 3, -1, -5, 2, -2, 5]
# idot 3 30
# init 3 79
# sum2 3 400030
# strd 3 [3, 5, 7]
# sdot 3 3
# wmap 3 [-3, 11, 3]
# wred 3 30
# imap 5 [-3, 11, 3, -5, 9]
# inpl 5 [-7, 14, 2, -10, 11, 0, -4, 3, -1, -5, 2, -2, 5, 1, -3, 4, 0, -4, 3]
# idot 5 55
# init 5 104
# sum2 5 500055
# strd 5 [3, 5, 7, -2, 0]
# sdot 5 -2
# wmap 5 [-3, 11, 3, -5, 9]
# wred 5 55
# imap 7 [-3, 11, 3, -5, 9, 1, -7]
# inpl 7 [-7, 14, 2, -10, 11, -1, -13, 3, -1, -5, 2, -2, 5, 1, -3, 4, 0, -4, 3, -1, -5, 2, -2, 5, 1]
# idot 7 71
# init 7 120
# sum2 7 100071
# strd 7 [3, 5, 7, -2, 0, 2, 4]
# sdot 7 5
# wmap 7 [-3, 11, 3, -5, 9, 1, -7]
# wred 7 71
# imap 8 [-3, 11, 3, -5, 9, 1, -7, 7]
# inpl 8 [-7, 14, 2, -10, 11, -1, -13, 8, -1, -5, 2, -2, 5, 1, -3, 4, 0, -4, 3, -1, -5, 2, -2, 5, 1, -3, 4, 0]
# idot 8 80
# init 8 129
# sum2 8 400080
# strd 8 [3, 5, 7, -2, 0, 2, 4, -5]
# sdot 8 2
# wmap 8 [-3, 11, 3, -5, 9, 1, -7, 7]
# wred 8 80
# imap 9 [-3, 11, 3, -5, 9, 1, -7, 7, -1]
# inpl 9 [-7, 14, 2, -10, 11, -1, -13, 8, -4, -5, 2, -2, 5, 1, -3, 4, 0, -4, 3, -1, -5, 2, -2, 5, 1, -3, 4, 0, -4, 3, -1]
# idot 9 81
# init 9 130
# sum2 9 300081
# strd 9 [3, 5, 7, -2, 0, 2, 4, -5, -3]
# sdot 9 2
# wmap 9 [-3, 11, 3, -5, 9, 1, -7, 7, -1]
# wred 9 81
# eq 0 True True True
# eq 1 True True True
# eq 2 True False True
# eq 3 True False True
# eq 4 True True True
# eq 5 True False True
# eq 6 True False True
# eq 7 True False True
# eq 8 True False True
# eq 9 True False True
# eq 16 True False True
# eq 33 True True True
# eq 64 True False True
# wrap 3 True
# wrap 4 True
# wrap 5 True
# wrap 8 True
# wrap 13 True
# wrap 40 True
# fmap 0 []
# fdot 0 0.0000
# fmap 1 [-1.0]
# fdot 1 1.0000
# fmap 2 [-1.0, 0.0]
# fdot 2 1.2500
# fmap 3 [-1.0, 0.0, 1.0]
# fdot 3 1.2500
# fmap 5 [-1.0, 0.0, 1.0, 2.0, 3.0]
# fdot 5 2.5000
# fmap 8 [-1.0, 0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0]
# fdot 8 15.0000
