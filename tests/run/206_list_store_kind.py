# 0.303 下标写入维护列表种类元数据回归。
# unknown 静态类型的写入必须升级 elem_kind / 更新 heap_bits：kind1 存标量、
# kind0 存堆值、kind2 位图按新值堆性更新（含跨 64 位字边界）；insert/del/
# reverse/pop/clear/切片赋值/extend/shuffle/sort 同族路径不得留下陈旧位图。
# 0.302 的 SIGSEGV：销毁把标量当堆指针 dec。ASan detect_leaks=1 验证升级后
# 堆元素真释放、环真收集。读回一律走 unknown 通道（静态元素类型特化是既有
# 语义，不在本修复范围）。

class C:
    pass

def setit(l, i, v):
    l[i] = v

def seti(l, i, v: int):
    l[i] = v

def seth(l, i, v: str):
    l[i] = v

def setf(l, v: float):
    l[0] = v

def readat(l, i):
    return l[i]

def sumlen(l: list[object]):
    s = 0
    for v in l:
        s += len(str(v))
    return s

def ident(x):
    return x

def ins(l: list[object], i, v):
    l.insert(i, v)

def rev(l: list[object]):
    l.reverse()

def clr(l: list[object]):
    l.clear()

def pop(l: list[object], i):
    return l.pop(i)

def dele(l: list[object], i):
    del l[i]

def slset(l, a, b, s):
    l[a:b] = s

def ext(l: list[object], s: list[object]):
    l.extend(s)

def app(l: list[object], v):
    l.append(v)

# ── 1) kind1 列表存标量（原始复现同族）：升级 kind2，销毁安全 ──
a = ["a"]
setit(a, 0, 1)
print(str(readat(ident(a), 0)))
b = [C()]
setit(b, 0, 2)
print(str(readat(ident(b), 0)))
# 负下标 + 越界
neg = ["p", "q"]
setit(neg, -1, 7)
print(str(readat(ident(neg), 1)))
try:
    setit(neg, 5, 1)
except IndexError:
    print("oob")
try:
    setit(neg, -5, 1)
except IndexError:
    print("oob2")

# ── 2) kind0 列表存堆值：升级 0→2 ──
c = [1, 2]
setit(c, 1, "s")
print(str(readat(ident(c), 1)), readat(ident(c), 0))
d = [0]
setit(d, 0, ident([9]))
print(len(readat(ident(d), 0)))

# ── 3) unknown 新值（运行期探针）：两个方向 ──
e = ["a"]
setit(e, 0, ident(3))
print(str(readat(ident(e), 0)))
f = [1]
setit(f, 0, ident("t"))
print(str(readat(ident(f), 0)))
g = ["a"]
setit(g, 0, ident("b"))
print(str(readat(ident(g), 0)))

# ── 4) kind2 位图：混合字面量，堆位↔标量位互换（探针/静态两条路） ──
m = [1, "x", 3]
setit(m, 1, 5)
setit(m, 0, "y")
print(str(readat(ident(m), 0)), readat(ident(m), 1), str(readat(ident(m), 2)))
m2 = [1, "x", 3]
seti(m2, 1, 8)
seth(m2, 0, "z")
print(str(readat(ident(m2), 0)), readat(ident(m2), 1))
print(sumlen(ident(m2)))

# ── 5) 跨 64 位字边界的位图更新 ──
big = [0, "s1", 2, "s3", 4, "s5", 6, "s7", 8, "s9", 10, "s11", 12, "s13", 14, "s15", 16, "s17", 18, "s19", 20, "s21", 22, "s23", 24, "s25", 26, "s27", 28, "s29", 30, "s31", 32, "s33", 34, "s35", 36, "s37", 38, "s39", 40, "s41", 42, "s43", 44, "s45", 46, "s47", 48, "s49", 50, "s51", 52, "s53", 54, "s55", 56, "s57", 58, "s59", 60, "s61", 62, "s63", 64, "s65", 66, "s67", 68, "s69", 70, "s71", 72, "s73", 74, "s75", 76, "s77", 78, "s79", 80, "s81", 82, "s83", 84, "s85", 86, "s87", 88, "s89", 90, "s91", 92, "s93", 94, "s95", 96, "s97", 98, "s99", 100, "s101", 102, "s103", 104, "s105", 106, "s107", 108, "s109", 110, "s111", 112, "s113", 114, "s115", 116, "s117", 118, "s119", 120, "s121", 122, "s123", 124, "s125", 126, "s127", 128, "s129"]
setit(big, 63, "w63")
setit(big, 64, 640)
setit(big, 65, "w65")
seti(big, 128, 1280)
seth(big, 129, "w129")
print(str(readat(ident(big), 63)), readat(ident(big), 64), str(readat(ident(big), 65)))
print(readat(ident(big), 128), str(readat(ident(big), 129)))
print(sumlen(ident(big)))

# ── 6) 浮点写入：kind1 升级；kind0 float 保持（nbody 快路径） ──
fl = ["a"]
setf(fl, 1.5)
fv: float = readat(ident(fl), 0)
print(fv)
fn = [0.0, 1.0]
setf(fn, 2.5)
gv: float = readat(ident(fn), 0)
print(gv)
fi = [1]
setf(fi, 3.5)
hv: float = readat(ident(fi), 0)
print(hv)

# ── 7) 大整数静态标量：绝不探针（位模式可能等于存活堆地址） ──
hi = ["a"]
seti(hi, 0, 70368744177664 + 12345)
print(readat(ident(hi), 0))

# ── 8) insert / del / pop / reverse / clear：位图丢弃同族 ──
t = [1, "x", 3]
ins(t, 1, 9)
print(len(t), readat(ident(t), 1))
ins(t, 0, "h")
print(str(readat(ident(t), 0)))
rev(t)
print(str(readat(ident(t), 0)))
p = pop(t, 0)
print(str(p))
dele(t, 0)
print(len(t))
clr(t)
print(len(t))
# kind1 insert 标量：升级
u = ["a"]
ins(u, 0, 4)
print(readat(ident(u), 0), str(readat(ident(u), 1)))

# ── 9) 切片赋值：增长 / 收缩 / 混合源 ──
s1 = [1, "x", 3]
slset(s1, 0, 2, [7, "z", 8])
print(len(s1), readat(ident(s1), 0), str(readat(ident(s1), 1)))
s2 = [1, "x", 3, "y", 5]
slset(s2, 1, 4, [6])
print(len(s2), readat(ident(s2), 1))
s3 = ["a"]
slset(s3, 0, 1, [1, 2])
print(len(s3), readat(ident(s3), 0))

# ── 10) extend / append：prep 升级与 kind3 定型 ──
x1 = ["a"]
ext(x1, [1, 2])
print(len(x1), readat(ident(x1), 1))
x2 = [1]
ext(x2, ["b"])
print(len(x2), str(readat(ident(x2), 1)))
x3 = []
app(x3, ident("s"))
app(x3, ident(2))
print(len(x3), str(readat(ident(x3), 0)), readat(ident(x3), 1))
x4 = ["a"]
app(x4, 5)
print(len(x4), readat(ident(x4), 1))

# ── 11) sort / shuffle：kind2 重排不得留陈旧位图 ──
srt = [1, "x"]
seth(srt, 0, "c")
seth(srt, 1, "a")
def dosort(l: list[object]):
    l.sort()
dosort(srt)
print(str(readat(ident(srt), 0)), str(readat(ident(srt), 1)))
import random
random.seed(7)
sh = [1, "x", 3, "y", 5]
random.shuffle(sh)
print(sumlen(ident(sh)))

# ── 12) churn：升级 + 销毁循环（LSan 抓漏释放） ──
def churn(n: int) -> int:
    s = 0
    for i in range(n):
        l = ["x"]
        setit(l, 0, i)
        l2 = [i]
        setit(l2, 0, ident("y"))
        m = [1, "x", 3]
        setit(m, 1, i)
        setit(m, 0, ident("y"))
        s += len(l) + len(l2) + len(m)
    return s
print(churn(20000))

# ── 13) GC 环：kind0 列表升级 0→2 后自环，必须被收集 ──
def mkcycle():
    l = [1]
    setit(l, 0, ident(l))
    l2 = ["a"]
    setit(l2, 0, ident(l2))
for _ in range(20000):
    mkcycle()
print("done")

# --- expect ---
# 1
# 2
# 7
# oob
# oob2
# s 1
# 1
# 3
# t
# b
# y 5 3
# z 8
# 3
# w63 640 w65
# 1280 w129
# 347
# 1.5
# 2.5
# 3.5
# 70368744190009
# 4 9
# h
# 3
# 3
# 3
# 0
# 4 a
# 4 7 z
# 3 6
# 2 1
# 3 1
# 2 b
# 2 s 2
# 2 5
# a c
# 5
# 100000
# done
