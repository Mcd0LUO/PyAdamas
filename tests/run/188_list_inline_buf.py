# 内联小列表缓冲（header + buffer 合并分配）的生命周期回归。
#
# 容量 <= 8 元素的列表，data 缓冲内联在「RC 头 + PyAdamasList」同一个
# malloc 块尾部（list_inline_pool）。本用例钉住所有会碰缓冲指针的路径：
#
#   1. append / insert / 切片赋值增长越过内联容量（8）：内联缓冲是块内
#      指针，不能 realloc，必须换 malloc + memcpy（list_buf_grow）；
#   2. 销毁时内联缓冲随头块整块挂回池、外部缓冲照旧还缓冲池 —— 高频
#      建/销下池复用不能读到 stale 指针（差分测试只比输出，这里特意让
#      堆元素跨复用存活）；
#   3. GC 清扫（gc_free_body）直接 free 内联列表的头块，不能单独 free
#      块内的 data 指针 —— 用自环列表 + 分配压力逼出清扫路径；
#   4. 堆元素（str / list）与混合种类照常走 retain/release 和位图；
#   5. 定长元组快路径（pa_list_new_filled / pa_tuple_new_filled）同样
#      走合并分配，kind 0 的 untracked 变体也要覆盖。

# ── 1. int 列表：从空增长越过 8 / 16 / 32 ──────────────────
a = []
i = 0
while i < 40:
    a.append(i * i)
    i += 1
print(len(a), a[0], a[7], a[8], a[15], a[16], a[39])
s = 0
for v in a:
    s += v
print(s)

# ── 2. str 列表（堆元素）：增长 + 高频建/销复用 ────────────
def build(n: int) -> list:
    out = []
    k = 0
    while k < n:
        out.append("s" + str(k))
        k += 1
    return out

big = build(20)
print(len(big), big[0], big[8], big[19])
t = 0
j = 0
while j < 3000:
    tmp = build(12)          # 每次都越过内联边界再销毁
    t += len(tmp[11])
    j += 1
print(t)

# ── 3. insert 增长（从内联容量起步）────────────────────────
ins = []
k = 0
while k < 12:
    ins.insert(0, k)
    k += 1
print(ins)

# ── 4. 切片赋值增长 ────────────────────────────────────────
sa = [1, 2, 3]
sa[1:2] = [9, 9, 9, 9, 9, 9, 9, 9, 9, 9]
print(len(sa), sa[0], sa[1], sa[10], sa[11])
sb = ["a", "b"]
sb[1:1] = ["x", "y", "z", "w", "v", "u", "t", "s"]
print(len(sb), sb[0], sb[9])

# ── 5. 混合种类 + 嵌套列表 ─────────────────────────────────
mix = []
mix.append(1)
mix.append("a" + "b")
print(mix[0], mix[1])
nest: list[list[int]] = []
i = 0
while i < 6:
    pair = [i, i + 1]
    nest.append(pair)
    i += 1
print(nest[0], nest[5], len(nest))

# ── 6. 元组快路径：kind 0（untracked）/ kind 1 / 混合 ──────
t0 = (1, 2)
t1 = ("a" + "b", "c" + "d")
t2 = (1, "x", 2, "y")
print(t0, t1, t2)
tt = ((1, 2), (3, 4))
print(tt[0], tt[1])

# ── 7. 大列表（>8）从头走外部缓冲 ──────────────────────────
lit = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]
print(len(lit), lit[8], lit[11])

# ── 8. 小列表的 copy / 切片 / extend / del / reverse / sort ─
c = [3, 1, 2]
d = c.copy()
d.append(4)
print(c, d)
e = c[:]
del e[0]
e.reverse()
print(e)
f = []
f.extend(c)
f.extend([7, 8, 9, 10, 11, 12])
print(len(f), f[0], f[8])
g = [5, 3, 9, 1]
g.sort()
print(g, sorted([5, 3, 9, 1], reverse=True))

# ── 9. 列表自环 + GC churn（gc_free_body 的内联路径）───────
keep = ["live", "alive"]
i = 0
while i < 20000:
    cyc = []
    cyc.append(cyc)          # 自环：RC 收不掉，靠 GC 清扫
    cyc.append("pad" + str(i))
    i += 1
print(keep[0], keep[1], i)

# ── 10. 堆元素小列表高频建/销（池复用正确性）───────────────
acc = 0
i = 0
while i < 5000:
    xs = ["k" + str(i % 97)]
    ys = [xs, xs]
    acc += len(ys[0][0])
    i += 1
print(acc)
# --- expect ---
# 40 0 49 64 225 256 1521
# 20540
# 20 s0 s8 s19
# 9000
# [11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0]
# 12 1 9 9 3
# 10 a b
# 1 ab
# [0, 1] [5, 6] 6
# (1, 2) ('ab', 'cd') (1, 'x', 2, 'y')
# (1, 2) (3, 4)
# 12 8 11
# [3, 1, 2] [3, 1, 2, 4]
# [2, 1]
# 9 3 12
# [1, 3, 5, 9] [9, 5, 3, 1]
# live alive 20000
# 14480
