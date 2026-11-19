# 248_p3_shadow_tag —— 0.327 P3 第一阶段：shadow tag 跟值走 + float 盒原位复用
#
# 门控模块（含 unknown 动态算术）里，unknown 标量槽/全局/vreg 带平行 tag：
# RC 免探针、动态算子按 tag 分派、累加器 float 盒原位复用（rc==1 校验）。
# tag 丢失（容器无位图/字段/形参/生成器）一律回退探针，正确性不依赖 tag。
# 覆盖：
#   1) 累加器复用命中：acc = acc + x 循环（int/float 混合），值正确；
#   2) 复用安全边界：旧值别名（y = acc; acc = acc+1; print(y) 必须是旧值）、
#      共享盒（列表元素 rc>=2 不复用，元素不被改写）、acc = acc + acc；
#   3) 常量缓存盒（rc>=2）不被复用改写；
#   4) 异构 list 遍历 + isinstance + 累加（u1 形态，位图保活）；
#   5) tag 回退：dict 值（val_own=2 探针）、object 形参/返回、类字段、
#      生成器 yield unknown（resume 无 tag）；
#   6) 一元负复用、// % / 混合算术、比较、str 拼接；
#   7) 下标写入 unknown 值（kind2 位图精确维护）+ 负索引/越界冷路径。
# 全部先对拍 CPython 再钉 expect；O0/O1/O2 输出一致。

# ── 1) 累加器复用命中 ─────────────────────────────────────────────
xs: list[object] = [1, 2.5, 3, 4.5, 7]
acc: object = 0
i: int = 0
while i < 20:
    acc = acc + xs[i % 5]
    if xs[i % 5] == 2.5:
        acc = acc + 0.5
    i = i + 1
print(acc)

# ── 2) 复用安全边界 ───────────────────────────────────────────────
# 旧值别名：y 与 acc 共享同一盒（rc>=2）→ 不复用，y 保持旧值。
y: object = acc
acc = acc + 1
print(y)
print(acc)
# 列表元素盒 rc>=2：a 借用自列表，a+1 不得改写列表元素。
a: object = xs[1]
b: object = a + 10
print(a)
print(xs[1])
print(b)
# acc = acc + acc：同一 vreg 双使用 → 不复用，值正确。
acc2: object = 1.5
acc2 = acc2 + acc2
print(acc2)
acc2 = acc2 + acc2
print(acc2)

# ── 3) 常量缓存盒不被改写 ─────────────────────────────────────────
c1: object = 1.25
c2: object = 1.25
c1 = c1 + 1
print(c1)
print(c2)

# ── 4) 异构 list 遍历 + isinstance + 累加（u1 形态）────────────────
ys: list[object] = []
k: int = 0
while k < 30:
    ys.append(1.5)
    ys.append(k)
    k = k + 1
total: object = 0.0
nf: int = 0
for v in ys:
    if isinstance(v, float):
        total = total + v
        nf = nf + 1
print(total)
print(nf)
print(len(ys))

# ── 5) tag 回退路径 ───────────────────────────────────────────────
# dict 值：val_own=2，取出无 tag → 探针回退。
d: dict[str, object] = {"f": 2.5, "i": 4, "s": "x"}
t: object = 0
for key in d:
    t = t + d[key] if key != "s" else t
print(t)
print(d["f"] * 2)
print(d["i"] // 3)
print(d["i"] % 3)

# object 形参/返回：调用边界 tag 不过（第一阶段），探针回退。
# 结果先落 object 槽再打印（247 同款）：直接 print(call) 形态命中
# 0.325 基线已存在的 unknown 调用结果所有权洞（基线/本分支均泄漏
# 144B/2obj，IR 逐字相同），不在本阶段范围。
def bump(x: object, n: int) -> object:
    r: object = x
    j: int = 0
    while j < n:
        r = r + 1
        j = j + 1
    return r
v1: object = bump(2.5, 3)
print(v1)
v2: object = bump(1.5, 4)
print(v2)

# 类字段：unknown 字段装载无 tag → 探针回退。
class Box:
    v: object
    def __init__(self, v: object):
        self.v = v
bx = Box(3.5)
print(bx.v + 1)
bx.v = bx.v + 0.5
print(bx.v)

# 生成器 yield unknown：resume 无 tag，探针回退。
# 注意：多态 yield（float+int+str 混合）是 0.325 基线已存在的边界
# （yield 类型统一后异类值不 coerce，基线/本分支输出逐字相同），
# 不在本测试范围；这里用同构 float yield 覆盖回退路径。
def gen():
    yield 1.5
    yield 2.5
g = gen()
print(next(g))
print(next(g))

# ── 6) 一元负 / 混合算术 / 比较 / str 拼接 ─────────────────────────
neg: object = 2.5
neg = -neg
print(neg)
neg = -neg
print(neg)
print(xs[0] / 2)
print(xs[1] // 1.0)
print(xs[1] % 2.0)
print(xs[0] < xs[1])
print(xs[1] >= 2.5)
print(xs[0] == 1.0)
print(xs[2] != 3)
s1: object = "ab"
s2: object = "cd"
print(s1 + s2)
print(int(xs[1]))
print(float(xs[0]))

# ── 7) 下标写入 unknown + 冷路径 ──────────────────────────────────
zs: list[object] = [1, 2, 3]
w: object = 9.5
zs[1] = w
print(zs)
print(zs[-1])
zs[-2] = 7
print(zs)
print(zs[0] + zs[1])


# --- expect ---
# 74.0
# 74.0
# 75.0
# 2.5
# 2.5
# 12.5
# 3.0
# 6.0
# 2.25
# 1.25
# 45.0
# 30
# 60
# 6.5
# 5.0
# 1
# 1
# 5.5
# 5.5
# 4.5
# 4.0
# 1.5
# 2.5
# -2.5
# 2.5
# 0.5
# 2.0
# 0.5
# True
# True
# True
# False
# abcd
# 2
# 1.0
# [1, 9.5, 3]
# 3
# [1, 7, 3]
# 8
