# 性能验收：len 必须 O(1)，顺序迭代必须线性（不能出现 O(n^2)）
import time

# 1) len 与串长无关：同一批循环里混取长短串的长度，耗时应与串长无关
small = "x" * 8
big = "x" * 100000
cjk = "中" * 50000

t0 = time.perf_counter()
acc = 0
for i in range(200000):
    acc += len(small) + len(big) + len(cjk)
t1 = time.perf_counter()
len_ms = (t1 - t0) * 1000.0
print("len_total", acc)
print("len_o1", len_ms < 5000.0)

# 2) len 对同一对象重复取值：每次都是常数时间（读偏移 0）
t0 = time.perf_counter()
v = 0
for i in range(500000):
    v += len(cjk)
t1 = time.perf_counter()
print("len_repeat", v)
print("len_repeat_ok", (t1 - t0) < 5.0)

# 3) 顺序迭代线性：N 与 2N 的耗时比必须接近 2（< 2.6 视为线性）
def iter_ms(s):
    t0 = time.perf_counter()
    c = 0
    for ch in s:
        c += 1
    t1 = time.perf_counter()
    return (t1 - t0) * 1000.0, c

n1 = 30000
n2 = 60000
s1 = "中文abc" * n1
s2 = "中文abc" * n2
m1, c1 = iter_ms(s1)
m2, c2 = iter_ms(s2)
print("iter_counts", c1, c2)
print("iter_linear", c2 == c1 * 2)
# 斜率判据：2N 不超过 N 的 2.6 倍（朴素 O(n^2) 会是 ~4 倍）
print("iter_slope_ok", m2 < m1 * 2.6 + 5.0)

# 4) 生成器 / enumerate / 推导式同样线性
def gen(s):
    for ch in s:
        yield ch

def gen_ms(s):
    t0 = time.perf_counter()
    c = 0
    for ch in gen(s):
        c += 1
    t1 = time.perf_counter()
    return (t1 - t0) * 1000.0, c

g1, gc1 = gen_ms(s1)
g2, gc2 = gen_ms(s2)
print("gen_counts", gc1, gc2)
print("gen_slope_ok", g2 < g1 * 2.6 + 5.0)

# 5) 切片不出现平方级退化（全串切片应共享原串，O(1)）
t0 = time.perf_counter()
for i in range(20000):
    t = s1[:]
t1 = time.perf_counter()
print("slice_full_ok", (t1 - t0) < 5.0)

# --- expect ---
# len_total 30001600000
# len_o1 True
# len_repeat 25000000000
# len_repeat_ok True
# iter_counts 150000 300000
# iter_linear True
# iter_slope_ok True
# gen_counts 150000 300000
# gen_slope_ok True
# slice_full_ok True
