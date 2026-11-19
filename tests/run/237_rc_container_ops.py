# 237_rc_container_ops —— 0.315：四个容器 RC / 语义洞（N10/N16/N17/N18）
#
# 0.314 及以前：
#   * N10  list += / *= 段错误（降级走 bin.add，后端按指针算术兜底）
#   * N16  dict.setdefault(k, []) 双重释放（默认值临时与返回值 RC 账不对）
#   * N17  s: set[str] = set() 不认标注：动态串不去重、退出时 RC 崩
#   * N18  sorted(xs, key=返堆 str) 规模化双重释放（键表没给堆键补引用）
# 本用例对拍 CPython，O0/O1/O2 全一致。

# 1) list 原地 += / *=（CPython __iadd__/__imul__：同一个对象）
l: list[int] = [1, 2]
l += [3, 4]
print(l)
m: list[int] = [1, 2]
m *= 2
print(m)
# 别名也看得见（原地语义）
a: list[int] = [1]
alias: list[int] = a
a += [2, 3]
print(alias)
# tuple 右操作数（list.__iadd__ 吃任意 iterable 的 list 形态）
c: list[int] = [1, 2]
c += (5, 6)
print(c)
# 0 / 负数倍数清空
d: list[int] = [1, 2]
d *= 0
print(d)
e: list[int] = [1, 2]
e *= -3
print(e)
# 堆元素重复：引用要跟着加
f: list[str] = ["a"]
falias: list[str] = f
f *= 3
print(falias)
g: list[int] = []
g *= 4
print(g)
# 循环里反复原地追加
h: list[int] = []
for i in range(4):
    h += [i * i]
print(h)

# 2) dict.setdefault：命中返回既有值、未命中插入并返回默认值
d2: dict[str, list[int]] = {}
d2.setdefault("k", []).append(1)
d2.setdefault("k", []).append(2)
print(d2["k"])
# 绑定到变量（命中路径）
v2: list[int] = d2.setdefault("k", [9])
print(v2)
# 未命中路径：返回的就是刚插入的那个对象
v3: list[int] = d2.setdefault("z", [7, 8])
v3.append(9)
print(d2["z"])
# 默认值不是字面量（直接引用一个已存在的列表）
shared: list[int] = [4]
d2.setdefault("w", shared)
print(d2["w"])
print(len(d2))
# 标量默认值照旧
d3: dict[str, int] = {}
print(d3.setdefault("a", 5))
print(d3.setdefault("a", 6))
print(d3)
# defaultdict 对照
import collections
dd = collections.defaultdict(list)
dd["q"].append(1)
dd["q"].append(2)
print(dd["q"])

# 3) 空 set[str] / dict[str, *] 走标注元素类型：动态串要按内容去重
s: set[str] = set()
x1: str = "x" + "y"
x2: str = "x" + "y"
s.add(x1)
s.add(x2)
s.add("ab" + "c")
print(len(s), sorted(s))
# dict() 空构造同样认标注
dd2: dict[str, int] = dict()
dd2["p" + "q"] = 1
dd2["pq"] = 2
print(dd2)
# 有元素的构造 / 推导式（既有路径）对照
print(sorted(set([x1, x2, "q" + "r"])))
print(sorted({t + "!" for t in ["m", "m", "n"]}))
print(len(s))

# 4) sorted(key=返回堆 str)：规模化（键表堆键的 RC 账）
def idk(t: str) -> str:
    return t

rows: list[str] = []
i: int = 0
while i < 2000:
    rows.append("s" + str((i * 37) % 2000))
    i += 1
ordered: list[str] = sorted(rows, key=idk)
print(len(ordered), ordered[0], ordered[1999])
# 小规模 + 稳定性（key 相等时保持原相对顺序）
pairs: list[str] = ["bb", "a", "cc", "d", "ee"]
def first(t: str) -> str:
    return t[0:1]

print(sorted(pairs, key=first))
print(sorted(pairs, key=first, reverse=True))
print(sorted(pairs, key=len))
# key 返回元组：kind-4 字典序比较路径（0.312 实现，0.315 起可用）
def pair(t: str) -> tuple[int, str]:
    return (len(t), t)

print(sorted(pairs, key=pair))
# 自然排序对照
print(sorted(rows)[0], sorted(rows)[1999])
print(rows[0], rows[1999])

# 5) set / dict 的原地集合运算（&= -= ^= |=）：后端没有 set 的 bin 运算，
#    落到整数路径会把哈希表指针当整数算（`s |= {2}` 曾打出 set() 并 rc 崩）
st: set[int] = {1, 2, 3}
st |= {2, 4}
print(sorted(st))
st &= {2, 4, 9}
print(sorted(st))
st -= {4}
print(sorted(st))
st ^= {2, 5}
print(sorted(st))
# 对称差双向：交集移除 + 差集补入（同一趟里做，别先删后补）
sa: set[int] = {1, 2}
sb: set[int] = {2, 3}
sa ^= sb
print(sorted(sa))
# 空集参与
se: set[int] = set()
se |= {7}
print(sorted(se))
se &= {8}
print(sorted(se))
# 堆元素
ss: set[str] = {"a" + "b", "c"}
ss |= {"a" + "b", "d"}
ss -= {"c"}
print(len(ss), sorted(ss))
# dict |=
dm: dict[str, int] = {"a": 1}
dm |= {"b": 2}
print(dm)
# set.update 对照（既有路径）
su: set[int] = {1}
su.update({1, 2})
print(sorted(su))

# --- expect ---
# [1, 2, 3, 4]
# [1, 2, 1, 2]
# [1, 2, 3]
# [1, 2, 5, 6]
# []
# []
# ['a', 'a', 'a']
# []
# [0, 1, 4, 9]
# [1, 2]
# [1, 2]
# [7, 8, 9]
# [4]
# 3
# 5
# 5
# {'a': 5}
# [1, 2]
# 2 ['abc', 'xy']
# {'pq': 2}
# ['qr', 'xy']
# ['m!', 'n!']
# 2
# 2000 s0 s999
# ['a', 'bb', 'cc', 'd', 'ee']
# ['ee', 'd', 'cc', 'bb', 'a']
# ['a', 'd', 'bb', 'cc', 'ee']
# ['a', 'd', 'bb', 'cc', 'ee']
# s0 s999
# s0 s1963
# [1, 2, 3, 4]
# [2, 4]
# [2]
# [5]
# [1, 3]
# [7]
# []
# 2 ['ab', 'd']
# {'a': 1, 'b': 2}
# [1, 2]
