# 元组的 repr：CPython 打印 (1, 2)，单元素带尾逗号 (1,)。
t = (1,)
print(t)
print(repr(t))
print(str(t))
print((1, 2), repr((1, 2)))
print(repr(()), repr((1.5,)), repr(('a',)), repr((True,)))
print(bool([]), bool([1]), bool({}), bool((1,)))
print([1, 2], (3,), {'k': (4,)})

# --- expect ---
# (1,)
# (1,)
# (1,)
# (1, 2) (1, 2)
# () (1.5,) ('a',) (True,)
# False True False True
# [1, 2] (3,) {'k': (4,)}
