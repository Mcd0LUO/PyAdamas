# 246_unknown_rc_re_empty —— 0.323：unknown 槽所有权（双 inc 泄漏）+ re 空匹配推进
#
# 两部分都用 ASan detect_leaks=1 抽查（套件默认 detect_leaks=0）：
#   1) unknown 元素写入槽 / for 循环变量 / 全局 / 字段 / unknown 形参：
#      旧口径下全局与字段每次写入连发两次 pa_rc_inc_unknown，槽类路径
#      则是「写入 retain、出口不释放」；字符串变索引叶子后都是可见泄漏。
#   2) re.findall / re.finditer 的空匹配推进：上一轮为空匹配时下一轮从
#      同一位置重搜，并拒绝同位置的空匹配（CPython 3.7+），否则会漏掉
#      同起点的非空分支。
import re

# ── 1) unknown 槽 / for 变量（局部）───────────────────────────────
def local_unknown() -> None:
    xs: list[object] = []
    i: int = 0
    while i < 200:
        xs.append("local-" + str(i) + "-xxxxxxxx")
        i = i + 1
    s: object = xs[0]
    t: object = xs[199]
    print(s)
    print(t)
    total: int = 0
    for v in xs:
        total = total + len(str(v))
    print(total)
    last: object = xs[100]
    print(last)

local_unknown()

# ── 2) unknown 全局（store_global 双 inc 路径）─────────────────────
GS: list[object] = []
j: int = 0
while j < 150:
    GS.append("global-" + str(j) + "-yyyyyyyy")
    j = j + 1
GV: object = GS[0]
print(GV)
for gv in GS:
    if len(str(gv)) < 10:
        print("bad")
print(GS[149])

# ── 3) unknown 字段（store_field 双 inc 路径）──────────────────────
# 类级标注让字段静态类型保持 unknown（写进 __init__ 的 `self.v: object = 0`
# 会被类型检查按首赋值的 int 收窄，测不到 unknown 字段路径）。
class Box:
    v: object

b: Box = Box()
b.v = GS[149]
print(b.v)
b.v = 42
print(b.v)

# ── 4) unknown 形参（入口 retain / 出口 release 对称）──────────────
def pick(x: object) -> object:
    if str(x) == "none":
        return 1
    return x

a: object = pick(GS[3])
print(a)
c: object = pick(42)
print(c)

# ── 5) re.findall / finditer 空匹配推进 ──────────────────────────
print(re.findall(r"\b|a", "ab"))
print(re.findall(r"|a", "ab"))
print(re.findall(r"a*|b", "ab"))
print(re.findall(r"\b|ab", "ab"))
print(re.findall(r"(?=a)|a", "ab"))
print(re.findall(r"(?=a)a", "aab"))
print(re.findall(r"(?<=a)b", "aab"))
print(re.findall(r"\b|.", "ab"))
print(re.findall(r"(\b)+", "ab"))
print(re.findall(r"(a*)+", "aaa"))
print(re.findall(r"x*|y", "xaby"))
spans: list[tuple[int, int]] = []
for m in re.finditer(r"\b|a", "ab"):
    spans.append(m.span())
print(spans)
spans2: list[tuple[int, int]] = []
for m in re.finditer(r"a*|b", "ab"):
    spans2.append(m.span())
print(spans2)
print(re.findall(r"^", ""))
print(re.findall(r"\A", ""))
print(re.findall(r"$", ""))
print(re.findall(r"", "ab"))
print(re.findall(r"\b", "ab"))
# --- expect ---
# local-0-xxxxxxxx
# local-199-xxxxxxxx
# 3490
# local-100-xxxxxxxx
# global-0-yyyyyyyy
# global-149-yyyyyyyy
# global-149-yyyyyyyy
# 42
# global-3-yyyyyyyy
# 42
# ['', 'a', '']
# ['', 'a', '', '']
# ['a', '', 'b', '']
# ['', 'ab', '']
# ['', 'a']
# ['a', 'a']
# ['b']
# ['', 'a', 'b', '']
# ['', '']
# ['', '']
# ['x', '', '', '', 'y', '']
# [(0, 0), (0, 1), (2, 2)]
# [(0, 1), (1, 1), (1, 2), (2, 2)]
# ['']
# ['']
# ['']
# ['', '', '']
# ['', '']
