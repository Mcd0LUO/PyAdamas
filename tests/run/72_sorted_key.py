# 排序与简单容器表示：sorted(key=)、tuple repr、identity
# 0.381 合并自 3 个文件：72_sorted_key.py, 75_tuple_repr.py, 87_identity.py

# ── 原 72_sorted_key.py ──────────────────────────────────────

xs = [3, 1, 2]
print(sorted(xs))
print(sorted(xs, key=lambda x: -x))
print(sorted(xs, reverse=True))
print(sorted(xs, key=lambda x: x, reverse=True))
words = ["bb", "a", "ccc"]
print(sorted(words, key=len))
print(xs)
pairs = [(2, "b"), (1, "a")]
print(sorted(pairs, key=lambda p: p[0]))

# ── 原 75_tuple_repr.py ──────────────────────────────────────

# 元组的 repr：CPython 打印 (1, 2)，单元素带尾逗号 (1,)。
t = (1,)
print(t)
print(repr(t))
print(str(t))
print((1, 2), repr((1, 2)))
print(repr(()), repr((1.5,)), repr(('a',)), repr((True,)))
print(bool([]), bool([1]), bool({}), bool((1,)))
print([1, 2], (3,), {'k': (4,)})

# ── 原 87_identity.py ──────────────────────────────────────

# is / is not 是同一性比较：比指针，不比内容
s = "abc"
print(s is None)
print(s is not None)

xs = [1, 2]
ys = xs
zs = [1, 2]
print(xs is ys)
print(xs is zs)
print(xs is not zs)

t = None
print(t is None)

n = 5
print(n is None)
print(n is not None)

d = {"k": 1}
print(d is None)
print(d is d)

empty = ""
print(empty is None)

# --- expect ---
# [1, 2, 3]
# [3, 2, 1]
# [3, 2, 1]
# [3, 2, 1]
# ['a', 'bb', 'ccc']
# [3, 1, 2]
# [(1, 'a'), (2, 'b')]
# (1,)
# (1,)
# (1,)
# (1, 2) (1, 2)
# () (1.5,) ('a',) (True,)
# False True False True
# [1, 2] (3,) {'k': (4,)}
# False
# True
# True
# False
# True
# True
# False
# True
# False
# True
# False
