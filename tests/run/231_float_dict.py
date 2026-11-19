# float 值 dict 的 get/下标存储/累加/覆盖/推导/拷贝（0.313 修复回归）
# 根因：dict 取值运行时统一返回 int64 机器字（float 按位模式）于 %rax，
# codegen 却按 float 静态类型读 %xmm0 —— get 对存在键返默认值、下标读回
# xmm0 残留（非字面量 float「存储后读回损坏」实为读路径坏，存储逐位正确）。
# 本用例钉住 DictGet/DictGetOr/DictPop/DictPopLast 全取值族的读回，O0/O1/O2 对拍。

# ── get：存在键 / 缺失键 / 默认值 ──
d: dict[str, float] = {}
d["x"] = 1.5
print(d["x"])
print(d.get("x", 0.0))
print(d.get("nope", 9.25))
print(d.get("x"))
print(d.get("nope"))
print(d.get("nope") is None)

# 非字面量默认值
fallback = -2.5
print(d.get("x", fallback))
print(d.get("nope", fallback))
print(d.get("nope2", 3.0))

# ── 非字面量 float 下标存储后读回 ──
def store_read(x: float) -> float:
    t: dict[str, float] = {}
    t["k"] = x
    y = x * 2.0  # 改变浮点寄存器残留：读回必须不受后续算术影响
    return t["k"] + y

print(store_read(1.5))
print(store_read(-3.25))
print(store_read(0.0))

def store_read_var(k: str, x: float) -> float:
    t: dict[str, float] = {}
    t[k] = x
    if k in t:
        return t[k]
    return -1.0

print(store_read_var("q", 9.5))
print(store_read_var("r", 0.125))

# ── 累加循环（contains+get 与 get 默认值两种写法）──
def accumulate(lat: float, paths: list[str]) -> dict[str, float]:
    lat_sum: dict[str, float] = {}
    for path in paths:
        if path in lat_sum:
            lat_sum[path] = lat_sum[path] + lat
        else:
            lat_sum[path] = lat
    return lat_sum

r = accumulate(0.25, ["a", "b", "a", "a", "b"])
print(r["a"], r["b"])
print(r.get("a", -1.0), r.get("b", -1.0), r.get("zz", -1.0))

def accumulate_get(lat: float, paths: list[str]) -> dict[str, float]:
    acc: dict[str, float] = {}
    for path in paths:
        acc[path] = acc.get(path, 0.0) + lat
    return acc

g = accumulate_get(0.5, ["p", "q", "p", "p", "q", "r"])
print(g["p"], g["q"], g["r"])

# ── float 值覆盖 ──
ow: dict[str, float] = {"a": 1.0}
ow["a"] = 2.5
print(ow["a"], ow.get("a", 0.0))
ow["a"] = ow["a"] * 2.0
print(ow["a"])
ow["a"] = -0.5
print(ow["a"], len(ow))

# ── 推导 / 拷贝 ──
comp: dict[str, float] = {k: len(k) * 0.5 for k in ["a", "bb", "ccc"]}
print(comp["a"], comp["bb"], comp["ccc"])
cp = comp.copy()
cp["a"] = 9.5
print(comp["a"], cp["a"], cp.get("bb", -1.0))
print(len(cp))

# ── pop / setdefault ──
pd: dict[str, float] = {"a": 2.5, "b": 7.5}
p = pd.pop("a")
print(p, len(pd))
q = pd.pop("b", -1.0)
print(q)
r2 = pd.pop("nope", -2.5)
print(r2, len(pd))
pd["c"] = 1.25
sd = pd.setdefault("c", 9.0)
print(sd)
sd2 = pd.setdefault("d", 3.5)
print(sd2, pd["d"], len(pd))

# ── values / items 迭代 ──
it: dict[str, float] = {"a": 0.5, "b": 1.5, "c": 2.5}
total = 0.0
for v in it.values():
    total = total + v
print(total)
for k, v in it.items():
    print(k, v)

# ── 跨函数借用读 ──
def read_key(dd: dict[str, float], k: str) -> float:
    return dd.get(k, -9.5)

print(read_key(it, "b"), read_key(it, "zz"))

# ── defaultdict(float) ──
import collections
ddf = collections.defaultdict(float)
ddf["v"] += 1.5
ddf["v"] += 0.25
print(ddf["v"], ddf.get("v", -1.0), ddf.get("w", -1.0), len(ddf))

# ── float 键 ──
fk: dict[float, str] = {1.5: "a", 2.5: "b"}
print(fk[1.5], fk.get(2.5, "?"), fk.get(9.5, "?"))
fk[3.5] = "c"
print(fk[3.5], len(fk))
fk2: dict[float, float] = {}
fk2[0.5] = 1.25
print(fk2[0.5], fk2.get(0.5, 0.0), fk2.get(0.25, -1.0))

# ── set[float].pop（无序：只钉成员资格与长度）──
sf: set[float] = {1.5, 2.5, 3.5}
x = sf.pop()
print(x == 1.5 or x == 2.5 or x == 3.5, len(sf))

# ── int 值 dict 对照（必须不受影响）──
di: dict[str, int] = {}
di["x"] = 3
print(di["x"], di.get("x", 0), di.get("nope", 7))
di["x"] = di["x"] + 4
print(di["x"])

# ── 大表触发 reindex 后读回 ──
big: dict[str, float] = {}
i = 0
while i < 40:
    big["k" + str(i)] = i * 0.25
    i = i + 1
s = 0.0
j = 0
while j < 40:
    s = s + big["k" + str(j)]
    j = j + 1
print(s, big.get("k39", -1.0), big.get("absent", -1.0))

# ── f-string 格式化读回 ──
print(f"{ow['a']:.2f} {d.get('x', 0.0):.1f}")
# --- expect ---
# 1.5
# 1.5
# 9.25
# 1.5
# None
# True
# 1.5
# -2.5
# 3.0
# 4.5
# -9.75
# 0.0
# 9.5
# 0.125
# 0.75 0.5
# 0.75 0.5 -1.0
# 1.5 1.0 0.5
# 2.5 2.5
# 5.0
# -0.5 1
# 0.5 1.0 1.5
# 0.5 9.5 1.0
# 3
# 2.5 1
# 7.5
# -2.5 0
# 1.25
# 3.5 3.5 2
# 4.5
# a 0.5
# b 1.5
# c 2.5
# 1.5 -9.5
# 1.75 1.75 -1.0 1
# a b ?
# c 3
# 1.25 1.25 -1.0
# True 2
# 3 3 7
# 7
# 195.0 9.75 -1.0
# -0.50 1.5
