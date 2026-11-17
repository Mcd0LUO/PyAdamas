# 切片拷贝惯用法（for k in range(lo,hi): dst.append(src[k]) → pa_list_copy_range）
#
# 钉住批量版与逐元素循环的语义等价：
#   1. 正常拷贝：int / float / str / tuple 元素，跨内联容量边界（8/9/20）；
#   2. 循环变量终值：正常结束 k == hi-1；零迭代保持 k 原值；
#   3. 慢路径：负索引归一化、越界 IndexError（部分结果保留）、lo > hi；
#   4. dst == src 别名、非空 dst 追加、全局 dst/src、裸列表（kind 3）定型、
#      混合种类（kind 2）逐元素探测；
#   5. 求值顺序：range 实参的副作用先于循环体，且只求值一次；
#   6. 不匹配的形态退回原循环：步长 2、双语句体、元组源、try 内、全局 k；
#   7. 生成器里可用；堆元素高频建/销 + GC 压力。
#
# 注：混合列表 repr 不给 str 加引号、unknown 里的 list 按整数打印，都是
# 既有行为（与惯用法无关，已用非匹配形态对照确认），这里避开这两种打印。

data = [10, 20, 30, 40, 50, 60, 70, 80, 90, 100]

# ── 1. 正常拷贝，跨内联边界 ──
def copy_range(src: list[int], lo: int, hi: int) -> list:
    out: list[int] = []
    for k in range(lo, hi):
        out.append(src[k])
    return out

def copy_strs(src: list[str], lo: int, hi: int) -> list:
    out: list[str] = []
    for k in range(lo, hi):
        out.append(src[k])
    return out

c8 = copy_range(data, 0, 8)
c9 = copy_range(data, 1, 10)
data2 = data.copy()
data2.extend(data)
c20 = copy_range(data2, 0, 20)
print(len(c8), c8[0], c8[7])
print(len(c9), c9[0], c9[8])
print(len(c20), c20[9], c20[19])
strs = ["a", "b", "c", "d", "e", "f", "g", "h", "i", "j"]
cs = copy_strs(strs, 2, 9)
print(len(cs), cs[0], cs[6])
floats = [1.5, 2.5, 3.5, 4.5]
cf: list[float] = []
for k in range(0, 4):
    cf.append(floats[k])
print(cf)
tups = [(1, 2), (3, 4), (5, 6)]
ct: list[tuple[int, int]] = []
for k in range(0, 3):
    ct.append(tups[k])
print(ct[0], ct[2], len(ct))

# ── 2. 循环变量终值 ──
k = 999
out = []
for k in range(2, 6):
    out.append(data[k])
print(k, out)
k = 777
out2 = []
for k in range(5, 5):
    out2.append(data[k])
print(k, len(out2))
out3 = []
for k in range(3, 1):
    out3.append(data[k])
print(k, len(out3))

# ── 3. 慢路径：负索引 / 越界 / 部分结果 ──
neg = []
for k in range(-3, 2):
    neg.append(data[k])
print(neg)
acc = []
def fill(n: int) -> None:
    for k in range(0, n):
        acc.append(data[k])
try:
    fill(14)
except IndexError:
    print("caught", len(acc), acc[0], acc[9])

# ── 4. 别名 / 非空 dst / 裸列表定型 / 混合种类 ──
alias = [1, 2, 3]
for k in range(0, 3):
    alias.append(alias[k])
print(alias)
pre = [0]
for k in range(0, 4):
    pre.append(data[k])
print(pre)
def pick(i: int):
    if i % 2 == 0:
        return i
    return "s" + str(i)
mixed = []
for i in range(6):
    mixed.append(pick(i))
bare = []
for k in range(0, 6):
    bare.append(mixed[k])
print(bare[0], bare[2], bare[4], len(bare))
print(bare[1], bare[3], bare[5])

# ── 5. 求值顺序与副作用 ──
calls = []
def lo():
    calls.append("lo")
    return 1
def hi():
    calls.append("hi")
    return 4
out4 = []
for k in range(lo(), hi()):
    out4.append(data[k])
print(calls, out4)
def bump():
    data.append(99)
    return 3
out5 = []
for k in range(0, bump()):
    out5.append(data[k])
print(out5, len(data))

# ── 6. 不匹配形态退回原循环 ──
step2 = []
for k in range(0, 8, 2):
    step2.append(data[k])
print(step2)
two = []
tot = 0
for k in range(0, 4):
    two.append(data[k])
    tot += k
print(two, tot)
tup_src = (7, 8, 9)
from_tup = []
for k in range(0, 3):
    from_tup.append(tup_src[k])
print(from_tup)
inner = []
try:
    for k in range(0, 4):
        inner.append(data[k])
except IndexError:
    print("never")
print(inner, k)
for j in range(2, 6):
    acc.append(data[j])
print(j, len(acc))

# ── 7. 生成器 + 高频建/销 + GC 压力 ──
def gen():
    out = []
    for k in range(1, 4):
        out.append(data[k])
    yield out
    yield len(out)
g = gen()
got = next(g)
n2 = next(g)
print(len(got), got[0], got[2], n2)
total = 0
i = 0
while i < 4000:
    tmp = copy_strs(strs, 0, 9)
    total += len(tmp[8])
    i += 1
print(total)
keep = ["live"]
i = 0
while i < 8000:
    cyc = copy_strs(strs, 0, 3)
    cyc.append(cyc)
    i += 1
print(keep[0], i)
# --- expect ---
# 8 10 80
# 9 20 100
# 20 100 100
# 7 c i
# [1.5, 2.5, 3.5, 4.5]
# (1, 2) (5, 6) 3
# 5 [30, 40, 50, 60]
# 777 0
# 777 0
# [80, 90, 100, 10, 20]
# caught 10 10 100
# [1, 2, 3, 1, 2, 3]
# [0, 10, 20, 30, 40]
# 0 2 4 6
# s1 s3 s5
# ['lo', 'hi'] [20, 30, 40]
# [10, 20, 30] 11
# [10, 30, 50, 70]
# [10, 20, 30, 40] 6
# [7, 8, 9]
# [10, 20, 30, 40] 3
# 5 14
# 3 20 40 3
# 4000
# live 8000
