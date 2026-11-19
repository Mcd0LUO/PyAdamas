# 247_unknown_float_tag —— 0.324：unknown 域的 float 判别（CPYFLOAT 装箱）
#
# 静态类型已知的路径不装箱；只有真正推断不出具体类型的值（object 容器/
# 槽/形参/返回、异构字面量）在 float 进入 unknown 时装箱成 CPYFLOAT。
# 覆盖：
#   1) 异构 list/dict 里 float 的 print / repr / str / f-string；
#   2) unknown 上的算术、比较、一元负、int()/float() 转换；
#   3) unknown 形参/返回、object 字段、循环变量；
#   4) 空容器推断：append 先把元素类型定成 float 后，之前的 int 元素按
#      元素类型转换（这里用纯 float 避免与 CPython 的 int 显示差异混淆）；
#   5) csv.writer 混合行（str + int + float）逐字段 str()，不再崩、不打印
#      位模式；
#   6) 嵌套 list[object] / dict[str, object]（内层元素也是 boxed float）。
#
# 全部先对拍 CPython 3.14 再钉 expect；O0/O1/O2 输出一致。
import csv

# ── 1) 异构 list：float 打印 / str / f-string ─────────────────────
xs: list[object] = []
xs.append(2.5)
xs.append(1)
xs.append("s")
xs.append(-0.5)
print(xs)
print(xs[0])
print(str(xs[0]))
print(f"{xs[0]}")
print(f"{xs[1]}")
print(len(xs))

# ── 2) 异构 dict：float 值 ────────────────────────────────────────
d: dict[str, object] = {}
d["a"] = 2.5
d["b"] = 1
d["c"] = "x"
d["d"] = -0.25
print(d)
print(d["a"])
print(str(d["a"]))
print(f"{d['a']}")

# ── 3) unknown 上的算术 ──────────────────────────────────────────
print(d["a"] + 1)
print(d["a"] - 0.5)
print(d["a"] * 2)
print(d["a"] / 2)
print(d["a"] + d["b"])
print(d["b"] + d["a"])
print(-d["a"])
print(d["a"] // 0.5)
print(d["a"] % 1.0)

# ── 4) unknown 上的比较 ──────────────────────────────────────────
print(d["a"] == 2.5)
print(d["a"] != 2.5)
print(d["a"] < 3)
print(d["a"] <= 2.5)
print(d["a"] > 2)
print(d["a"] >= 2.5)
print(d["b"] == 1.0)
print(xs[2] == "s")
print(xs[2] + "!")

# ── 5) int() / float() / bool() ──────────────────────────────────
print(int(d["a"]))
print(float(d["b"]))
print(bool(d["a"]))
print(bool(xs[0] - 2.5))

# ── 6) unknown 形参 / 返回 / 字段 / 循环变量 ──────────────────────
def pick(x: object) -> object:
    return x

v: object = pick(3.5)
print(v)
print(v * 2)

class Box:
    v: object

b: Box = Box()
b.v = 4.5
print(b.v)
print(b.v + 0.5)

total: object = 0
for x in xs:
    if isinstance(x, float):
        total = total + x
print(total)

# ── 7) 空容器推断：先 append int、后 append float ──────────────────
ys = []
ys.append(1.5)
ys.append(2.5)
print(ys)
print(ys[0] + ys[1])

# ── 8) 嵌套 unknown 容器 ─────────────────────────────────────────
inner: list[object] = [2.5, "y"]
outer: list[object] = [inner, 1]
print(outer)
idct: dict[str, object] = {}
idct["k"] = 3.5
od: dict[str, object] = {}
od["inner"] = idct
od["n"] = 2
print(od)
print(od["inner"])

# ── 9) csv 混合行（str + int + float）────────────────────────────
p: str = "/tmp/pa247_csv.csv"
f = open(p, "w", newline="")
w = csv.writer(f)
w.writerow(["a", 1, 2.5])
row: list[object] = ["b", 2.5, 3]
w.writerow(row)
f.close()
g = open(p, "r", newline="")
body: str = g.read()
g.close()
print(repr(body))

# --- expect ---
# [2.5, 1, 's', -0.5]
# 2.5
# 2.5
# 2.5
# 1
# 4
# {'a': 2.5, 'b': 1, 'c': 'x', 'd': -0.25}
# 2.5
# 2.5
# 2.5
# 3.5
# 2.0
# 5.0
# 1.25
# 3.5
# 3.5
# -2.5
# 5.0
# 0.5
# True
# False
# True
# True
# True
# True
# True
# True
# s!
# 2
# 1.0
# True
# False
# 3.5
# 7.0
# 4.5
# 5.0
# 2.0
# [1.5, 2.5]
# 4.0
# [[2.5, 'y'], 1]
# {'inner': {'k': 3.5}, 'n': 2}
# {'k': 3.5}
# 'a,1,2.5\r\nb,2.5,3\r\n'
