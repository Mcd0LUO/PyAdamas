# sorted(key=返回用户类实例)：按 __lt__ 排序（旧实现按堆地址，静默不排序）。
# 同时覆盖 kind 3（key 返回类型推不出来）的数值/str 族、稳定性、TypeError，
# 以及 pa_list_sort 的 kind-3 实例路径（族分类抽成 uk_sort_family 后的回归）。

class P:
    x: int
    name: str
    def __init__(self, x: int, name: str) -> None:
        self.x = x
        self.name = name
    def __lt__(self, other: P) -> bool:
        return self.x < other.x
    def __repr__(self) -> str:
        return self.name

# 1) 构造顺序与期望顺序相反：旧实现按堆地址稳定返回原顺序
a: P = P(3, "a")
b: P = P(1, "b")
c: P = P(2, "c")
ps: list[P] = [a, b, c]

def ident(p: P) -> P:
    return p
print(sorted(ps, key=ident))
print(ps)

# 2) 返回类型没标注（kind 3）走同一条对象比较路径
def ident2(p: P):
    return p
print(sorted(ps, key=ident2))

# 3) key 每次新建实例（键对象的 RC/生命周期）
def fresh(p: P) -> P:
    return P(p.x, p.name)
print(sorted(ps, key=fresh))

# 4) 稳定性：相等 key 保持原相对顺序
ps2: list[P] = [P(2, "x"), P(1, "y"), P(2, "z"), P(1, "w")]
print(sorted(ps2, key=ident))

# 5) reverse=True
print(sorted(ps, key=ident, reverse=True))

# 6) kind 3 数值族：float 键不再按位模式/指针比
def half(v: int):
    return v / 2.0
print(sorted([3, 1, 2], key=half))
print(sorted([-3, 1, -2], key=half))

# 7) kind 3 str 族
def tail(s: str):
    return s[1:]
print(sorted(["za", "ab", "bc", "aa"], key=tail))

# 8) 没有 __lt__ 的类：TypeError，而不是静默按地址排
class Q:
    x: int
    def __init__(self, x: int) -> None:
        self.x = x

def qkey(v: int) -> Q:
    return Q(v)
try:
    sorted([2, 1], key=qkey)
    print("no error")
except TypeError:
    print("TypeError")

# 9) 大表（>64）稳定性：同键组内保持原顺序
big: list[P] = []
i: int = 0
while i < 200:
    big.append(P((i * 37) % 5, "p" + str(i)))
    i += 1
sb = sorted(big, key=ident)
ok: bool = True
j: int = 1
while j < len(sb):
    if sb[j].x < sb[j - 1].x:
        ok = False
    if sb[j].x == sb[j - 1].x:
        n0: int = int(sb[j - 1].name[1:])
        n1: int = int(sb[j].name[1:])
        if n1 < n0:
            ok = False
    j += 1
print(len(sb), sb[0].x, sb[-1].x, ok)

# 10) 原地 list.sort() 的 kind-3 实例路径（pa_list_sort）仍是 __lt__
raw: list[P] = [P(2, "m"), P(3, "n"), P(1, "o")]
raw.sort()
print(raw)
# --- expect ---
# [b, c, a]
# [a, b, c]
# [b, c, a]
# [b, c, a]
# [y, w, x, z]
# [a, c, b]
# [1, 2, 3]
# [-3, -2, 1]
# ['za', 'aa', 'ab', 'bc']
# TypeError
# 200 0 4 True
# [o, m, n]
