# 230_tuple_sort —— 0.312：元组列表排序（修复前 kind3 机器字分派是 no-op）
# 断言顺序：同质 int/str 元组、混合 int+str、float 元组、不同 arity（变长）、
# reverse=True、key 函数（int/str 键）、稳定性、嵌套元组（含三层）、空表/单元素、
# 大表（n>64 触发 timsort 归并 + 临时区缓冲池）。元组返回 key 的 RC 崩溃是
# 另一个既有 bug（0.271 oracle 同款复现），不在本用例范围，见 CHANGELOG 0.312。

# 1) 同质 int 元组：字典序（先第一元素，再第二元素）
a: list[tuple[int, int]] = [(2, 9), (1, 5), (2, 3), (1, 5), (-1, 0), (0, 7)]
a.sort()
print(a)

# 2) sorted() 是副本：原表不动
b: list[tuple[int, int]] = [(3, 1), (1, 2), (3, 0)]
sb = sorted(b)
print(sb)
print(b)

# 3) str 元组（字节序，大写在小写前）
c: list[tuple[str, str]] = [("b", "x"), ("a", "z"), ("a", "y"), ("B", "a")]
c.sort()
print(c)

# 4) 混合 int+str / str+int
d: list[tuple[int, str]] = [(2, "b"), (1, "a"), (2, "a"), (1, "z")]
d.sort()
print(d)
e: list[tuple[str, int]] = [("b", 2), ("a", 1), ("b", 1)]
print(sorted(e))

# 5) float 元组
f: list[tuple[float, int]] = [(2.5, 1), (1.5, 2), (2.5, 0)]
f.sort()
print(f)
m: list[tuple[int, float]] = [(1, 2.5), (0, 9.5), (1, 1.5)]
print(sorted(m))

# 6) 不同 arity（变长元组）：前缀相等短者小
v: list[tuple[int, ...]] = [(1, 2), (1,), (1, 2, 0), (0, 9), (1,)]
v.sort()
print(v)

# 7) reverse=True / False
r: list[tuple[int, int]] = [(1, 5), (2, 9), (2, 3)]
print(sorted(r, reverse=True))
print(sorted(r, reverse=False))

# 8) key 函数（int 键）：按第二元素排
k: list[tuple[int, int]] = [(1, 9), (2, 1), (3, 5)]
def by_second(t: tuple[int, int]) -> int:
    return t[1]
print(sorted(k, key=by_second))
print(sorted(k, key=by_second, reverse=True))

# 9) 稳定性：key 相等保持原相对顺序（int 键 / str 键）
s2: list[tuple[int, str]] = [(2, "a"), (1, "b"), (3, "a"), (0, "b")]
def tag(t: tuple[int, str]) -> str:
    return t[1]
print(sorted(s2, key=tag))
s3: list[tuple[int, str]] = [(2, "a"), (1, "b"), (2, "c"), (1, "d")]
def first(t: tuple[int, str]) -> int:
    return t[0]
print(sorted(s3, key=first))

# 10) key=len：变长元组按长度排（稳定）
w: list[tuple[int, ...]] = [(1, 2), (5,), (3, 4, 5), (9,)]
print(sorted(w, key=len))

# 11) 嵌套元组：内层也按字典序
n: list[tuple[tuple[int, int], int]] = [((2, 1), 0), ((1, 9), 5), ((2, 0), 1), ((1, 9), 2)]
n.sort()
print(n)
n2: list[tuple[int, tuple[str, int]]] = [(1, ("b", 2)), (0, ("z", 0)), (1, ("a", 9))]
print(sorted(n2))
n3: list[tuple[tuple[tuple[int, int], int], int]] = [(((2, 1), 0), 9), (((1, 9), 5), 0), (((2, 1), 0), 1)]
print(sorted(n3))

# 12) 空表 / 单元素
empty: list[tuple[int, int]] = []
empty.sort()
print(empty)
print(sorted(empty))
print(sorted(empty, reverse=True))
one: list[tuple[int, str]] = [(7, "solo")]
one.sort()
print(one)
print(sorted(one, reverse=True))
print(sorted([((1, 2), 3)]))

# 13) 降序输入（自然 run 反转）/ 近乎有序（二分插入）
dec: list[tuple[int, int]] = [(3, 3), (2, 2), (1, 1)]
dec.sort()
print(dec)
alm: list[tuple[int, int]] = [(1, 1), (2, 2), (4, 4), (3, 3), (5, 5)]
alm.sort()
print(alm)

# 14) 三元组（sort_struct 同款形状）
t3: list[tuple[int, int, int]] = [(1, 2, 3), (1, 2, 0), (0, 9, 9), (1, 0, 5)]
t3.sort()
print(t3)

# 15) tuple 输入（物化成 list 再排）
print(sorted(((2, 1), (1, 3))))

# 16) 大表：n=300 > 64 触发 timsort 归并 + 临时区入池
big: list[tuple[int, int]] = []
x = 7
for i in range(300):
    x = (x * 1103515245 + 12345) % 2147483648
    big.append((x % 50, i))
big.sort()
ok = 1
for i in range(1, 300):
    p0 = big[i - 1][0]
    p1 = big[i][0]
    if p0 > p1:
        ok = 0
    if p0 == p1 and big[i - 1][1] >= big[i][1]:
        ok = 0
print(ok, big[0], big[299])
# --- expect ---
# [(-1, 0), (0, 7), (1, 5), (1, 5), (2, 3), (2, 9)]
# [(1, 2), (3, 0), (3, 1)]
# [(3, 1), (1, 2), (3, 0)]
# [('B', 'a'), ('a', 'y'), ('a', 'z'), ('b', 'x')]
# [(1, 'a'), (1, 'z'), (2, 'a'), (2, 'b')]
# [('a', 1), ('b', 1), ('b', 2)]
# [(1.5, 2), (2.5, 0), (2.5, 1)]
# [(0, 9.5), (1, 1.5), (1, 2.5)]
# [(0, 9), (1,), (1,), (1, 2), (1, 2, 0)]
# [(2, 9), (2, 3), (1, 5)]
# [(1, 5), (2, 3), (2, 9)]
# [(2, 1), (3, 5), (1, 9)]
# [(1, 9), (3, 5), (2, 1)]
# [(2, 'a'), (3, 'a'), (1, 'b'), (0, 'b')]
# [(1, 'b'), (1, 'd'), (2, 'a'), (2, 'c')]
# [(5,), (9,), (1, 2), (3, 4, 5)]
# [((1, 9), 2), ((1, 9), 5), ((2, 0), 1), ((2, 1), 0)]
# [(0, ('z', 0)), (1, ('a', 9)), (1, ('b', 2))]
# [(((1, 9), 5), 0), (((2, 1), 0), 1), (((2, 1), 0), 9)]
# []
# []
# []
# [(7, 'solo')]
# [(7, 'solo')]
# [((1, 2), 3)]
# [(1, 1), (2, 2), (3, 3)]
# [(1, 1), (2, 2), (3, 3), (4, 4), (5, 5)]
# [(0, 9, 9), (1, 0, 5), (1, 2, 0), (1, 2, 3)]
# [(1, 3), (2, 1)]
# 1 (0, 10) (49, 281)
