# 245_regressions —— 0.322 第二批正确性修复（P0/P1）
#
# 覆盖：
#   * csv.writer 收 [str, int] 混合行（原来 list[str] 形参按 str 指针读 int，
#     SIGSEGV；现在 writerow 收 list[object] 并逐字段 str()）。
#   * re 零宽量化：`\b+` 按 CPython 报 nothing to repeat（原来无限递归
#     SIGSEGV）；`(\b)+` / `(?:\b)+` / `(a*)+` / `(?=a)+` 等合法形态匹配
#     CPython（循环体匹配空串时停止重复）。
#   * re 多捕获 findall：>= 2 组时每个元素是 tuple（原来推整段匹配）。
#   * type(e).__name__：except Exception 捕获后返回真实异常类名。
#   * 异构 dict repr：{str: int|str} 打印 `{'a': 1, 'b': 'x'}`（原来
#     `<dict object>`）。
#   * 非原地集合运算 s | t / s & t / s - t / s ^ t（原来「后端不支持」）。
#   * 生成器表达式元素类型：for v in (x for x in list[float]) 保留 float。
#   * N19b：被内联的浮点返回调用 + 循环读副本槽（O2 下 pay 读 0）。
#
# 全部先对拍 CPython 3.14 再钉 expect；O0/O1/O2 输出一致。

import csv
import math
import re

# ── 1) csv.writer 混合行（[str, int]）────────────────────────────
p: str = "/tmp/pa245_csv.csv"
f = open(p, "w", newline="")
w = csv.writer(f)
w.writerow(["x", 1])
w.writerow(["y", 3])
f.close()
g = open(p, "r", newline="")
body: str = g.read()
g.close()
print(body.replace("\r", "<CR>").replace("\n", "<LF>"))

# 行由 list[object] 变量给出（不是字面量）
row: list[object] = ["a", 7]
q: str = "/tmp/pa245_csv2.csv"
f2 = open(q, "w", newline="")
w2 = csv.writer(f2, lineterminator="\n")
w2.writerow(row)
w2.writerows([["m", "n"], ["o", "p"]])
f2.close()
g2 = open(q)
print(g2.read().replace("\n", "|"))
g2.close()

# ── 2) re 零宽量化 ──────────────────────────────────────────────
try:
    re.findall(r"\b+", "ab")
    print("no error")
except Exception:
    print("error")

try:
    re.findall(r"\b*", "ab")
    print("no error")
except Exception:
    print("error")

try:
    re.findall(r"\b{2}", "ab")
    print("no error")
except Exception:
    print("error")

print(re.findall(r"(\b)+", "ab"))
print(re.findall(r"(?:\b)+", "ab"))
print(re.findall(r"(?:\b)*", "ab"))
print(re.findall(r"(a*)+", "ab"))
print(re.findall(r"(?=a)+", "aab"))
print(re.findall(r"(?<=a)+", "aab"))
print(re.findall(r"a*", "ab"))
print(re.findall(r"(?:a|)*?b", "aab"))
print(re.findall(r"(a*)*?b", "aab"))
print(re.findall(r"(\b|b)", "ab"))

# ── 3) re 多捕获 findall（>= 2 组 → tuple）──────────────────────
print(re.findall(r"(a)(b)", "ab ab"))
print(re.findall(r"(a)?(b)", "b"))
print(re.findall(r"(a)|(b)", "ab"))
print(re.findall(r"(a)(b)?(c)", "abc ac"))
print(re.findall(r"([0-9]+)-([a-z]+)", "12-ab 34-cd"))
for k, v in re.findall(r"(\d+)-(\w+)", "1-x 2-y"):
    print("pair", k, v)
pairs: list[tuple[str, str]] = re.findall(r"(\d+)-(\w+)", "1-x")
print(pairs)
print(re.findall(r"(a)(b)(c)", "abc"))
print(re.findall(r"x", "x x"))
print(re.findall(r"(x)", "x x"))

# ── 4) type(e).__name__ ─────────────────────────────────────────
try:
    1 // 0
except Exception as e:
    print("zde", type(e).__name__)
try:
    {}["k"]
except Exception as e:
    print("key", type(e).__name__)
try:
    [1][5]
except Exception as e:
    print("idx", type(e).__name__)
try:
    raise ValueError("boom")
except Exception as e:
    print("val", type(e).__name__, str(e))
try:
    raise KeyError("a")
except KeyError as e:
    print("key2", type(e).__name__)
try:
    raise ValueError("x")
except Exception as e:
    print("wrap", type(e).__name__, str(e))
print(type(1).__name__, type("s").__name__, type(2.5).__name__)

# ── 5) 异构 dict repr ───────────────────────────────────────────
d = {"a": 1, "b": "x"}
print(d)
e2 = {"k": [1, 2], "m": {"n": 2}, "s": "t"}
print(e2)
empty: dict[str, object] = {}
print(empty)
fp: str = "/tmp/pa245_dict.txt"
hf = open(fp, "w")
print(d, file=hf)
hf.close()
hr = open(fp)
print(hr.read().replace("\n", "|"))
hr.close()

# ── 6) 非原地集合运算 ───────────────────────────────────────────
s: set[int] = {1, 2, 3}
t: set[int] = {3, 4}
print(sorted(s | t), sorted(s & t), sorted(s - t), sorted(s ^ t))
print(sorted(t | s), sorted(s | set()), sorted(s - set()))
u: set[str] = {"a", "b"}
print(sorted(u | {"c"}), sorted(u & {"b"}), sorted(u - {"a"}), sorted(u ^ {"b", "c"}))
print(len(s), len(t), s)

# ── 7) 生成器表达式元素类型（float）────────────────────────────
xs: list[float] = [1.5, 2.5]
for fv in (x for x in xs):
    print(fv)
print(sum(gx for gx in xs))
print([z for z in (x for x in xs)])

# ── 8) N19b：内联浮点返回调用 + 循环读副本槽 ────────────────────
def monthly_payment(principal: float, annual_rate: float, months: int) -> float:
    r: float = annual_rate / 12.0
    if r == 0.0:
        return principal / float(months)
    factor: float = math.pow(1.0 + r, float(months))
    num: float = principal * r * factor
    den: float = factor - 1.0
    return num / den

def amortize(principal: float, annual_rate: float, months: int) -> list[float]:
    r: float = annual_rate / 12.0
    pay: float = monthly_payment(principal, annual_rate, months)
    bal: float = principal
    interest_total: float = 0.0
    rows: list[float] = []
    for m in range(months):
        interest: float = bal * r
        principal_part: float = pay - interest
        bal = bal - principal_part
        interest_total += interest
        if m < 2 or m >= months - 2:
            rows.append(float(m + 1))
            rows.append(interest)
            rows.append(principal_part)
            rows.append(bal)
    rows.append(interest_total)
    rows.append(bal)
    return rows

print("pay", monthly_payment(300000.0, 0.045, 360))
for x in amortize(300000.0, 0.045, 360):
    print(x)

# --- expect ---
# x,1<CR><LF>y,3<CR><LF>
# a,7|m,n|o,p|
# error
# error
# error
# ['', '']
# ['', '']
# ['', '', '']
# ['', '', '']
# ['', '']
# ['', '']
# ['a', '', '']
# ['aab']
# ['aa']
# ['', 'b', '']
# [('a', 'b'), ('a', 'b')]
# [('', 'b')]
# [('a', ''), ('', 'b')]
# [('a', 'b', 'c'), ('a', '', 'c')]
# [('12', 'ab'), ('34', 'cd')]
# pair 1 x
# pair 2 y
# [('1', 'x')]
# [('a', 'b', 'c')]
# ['x', 'x']
# ['x', 'x']
# zde ZeroDivisionError
# key KeyError
# idx IndexError
# val ValueError boom
# key2 KeyError
# wrap ValueError x
# int str float
# {'a': 1, 'b': 'x'}
# {'k': [1, 2], 'm': {'n': 2}, 's': 't'}
# {}
# {'a': 1, 'b': 'x'}|
# [1, 2, 3, 4] [3] [1, 2] [1, 2, 4]
# [1, 2, 3, 4] [1, 2, 3] [1, 2, 3]
# ['a', 'b', 'c'] ['b'] ['b'] ['a', 'c']
# 3 2 {1, 2, 3}
# 1.5
# 2.5
# 4.0
# [1.5, 2.5]
# pay 1520.0559294776574
# 1.0
# 1125.0
# 395.0559294776574
# 299604.9440705223
# 2.0
# 1123.5185402644586
# 396.5373892131988
# 299208.4066813091
# 359.0
# 11.336611252060228
# 1508.7193182255971
# 1514.3770156571306
# 360.0
# 5.67891380871424
# 1514.3770156689432
# -1.1812517186626792e-08
# 247220.1346119449
# -1.1812517186626792e-08
