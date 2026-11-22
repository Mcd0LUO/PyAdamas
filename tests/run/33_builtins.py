# 内置函数综合：常用内建与边界
# 0.381 合并自 3 个文件：33_builtins.py, 64_builtins.py, 76_small_builtins.py

# ── 原 33_builtins.py ──────────────────────────────────────

print(abs(-5))
print(abs(-2.5))
print(min(3, 1, 2))
print(max(3, 1, 2))
print(min(2.5, 1.5))
print(max(2, 3.5))
print(min([5, 2, 9]))
print(max((5, 2, 9)))
print(min("banana"))
print(max("banana"))
print(sum([1, 2, 3]))
print(sum((1.5, 2.5)))
print(sum([], 10))
print(sum([1, 2], 10))
print(min({"b": 1, "a": 2}))
print(max({3, 1, 2}))
print(pow(2, 10))
print(pow(2.5, 2))
try:
    min([])
except ValueError as e:
    print("empty", e)
try:
    max(())
except ValueError as e:
    print("empty2", e)
print(any([0, 1, 2]))
print(any([]))
print(all([1, 2, 3]))
print(all([1, 0]))
print(all([]))
print(any(["", "a"]))
print(all(["a", "b"]))
print(any({"", "x"}))
print(all([True, False]))
print(any(["", ""]))

# ── 原 64_builtins.py ──────────────────────────────────────

print(list(map(str, [1, 2, 3])))
print(list(filter(lambda x: x > 1, [1, 2, 3])))
print(sorted([3, 1, 2]))
print(sorted([3, 1, 2], reverse=True))
print(any([0, 1]))
print(all([1, 1]))
print(divmod(7, 2))
print(pow(2, 10))
print(round(3.567, 2))
print(repr("a"))
print(format(3.14, ".1f"))
print(format(42, "05d"))
print(min([3, 1, 2]), max([3, 1, 2]))
print(sum([1, 2, 3]))
print(abs(-5))
print(list(zip([1, 2], ["a", "b"])))
print(list(enumerate(["a", "b"])))
print(hex(255), oct(8), bin(5))
print(chr(65), ord("A"))
print(isinstance(1, int), callable(print))
print(hash("x") == hash("x"))

# ── 原 76_small_builtins.py ──────────────────────────────────────

# 小内置：slice() 作下标、vars() 取实例字段、dir() 列成员
xs = [1, 2, 3, 4, 5]
print(xs[slice(1, 3)])
print(xs[slice(3)])
print(xs[slice(1, 5, 2)])
print(xs[slice(-2, None)])
print(xs[slice(None, 2)])

s = "abcdef"
print(s[slice(1, 4)])
print(s[slice(None, None, 2)])

xs[slice(0, 2)] = [9, 8]
print(xs)
del xs[slice(0, 1)]
print(xs)


class Point:
    x: int = 1
    y: int = 2

    def norm(self) -> int:
        return self.x + self.y


p = Point()
p.x = 10
d = vars(p)
print(len(d), d["x"], d["y"])
print(sorted(d.keys()))
print(dir(p))


def local_names():
    a = 1
    b = 2
    return dir()


print(local_names())
print(dir()[0:0])

# --- expect ---
# 5
# 2.5
# 1
# 3
# 1.5
# 3.5
# 2
# 9
# a
# n
# 6
# 4.0
# 10
# 13
# a
# 3
# 1024
# 6.25
# empty min() iterable argument is empty
# empty2 max() iterable argument is empty
# True
# False
# True
# False
# True
# True
# True
# True
# False
# False
# ['1', '2', '3']
# [2, 3]
# [1, 2, 3]
# [3, 2, 1]
# True
# True
# (3, 1)
# 1024
# 3.57
# 'a'
# 3.1
# 00042
# 1 3
# 6
# 5
# [(1, 'a'), (2, 'b')]
# [(0, 'a'), (1, 'b')]
# 0xff 0o10 0b101
# A 65
# True True
# True
# [2, 3]
# [1, 2, 3]
# [2, 4]
# [4, 5]
# [1, 2]
# bcd
# ace
# [9, 8, 3, 4, 5]
# [8, 3, 4, 5]
# 2 10 2
# ['x', 'y']
# ['norm', 'x', 'y']
# ['a', 'b']
# []
