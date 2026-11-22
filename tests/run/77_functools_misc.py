# functools 与迭代器助手：enumerate/zip/functools
# 0.381 合并自 2 个文件：77_functools.py, 78_enumerate_zip.py

# ── 原 77_functools.py ──────────────────────────────────────

# functools：reduce 折叠、wraps 装饰器（无运行期语义，装饰器链里跳过）
import functools


def add(a, b):
    return a + b


def mul(a, b):
    return a * b


def fadd(a: float, b: float) -> float:
    return a + b


def sadd(a: str, b: str) -> str:
    return a + b


print(functools.reduce(add, [1, 2, 3, 4]))
print(functools.reduce(add, [1, 2, 3], 10))
print(functools.reduce(mul, [2, 3, 4]))
print(functools.reduce(add, (5, 6, 7)))
print(functools.reduce(fadd, [1.5, 2.5]))
print(functools.reduce(sadd, ["a", "b", "c"]))
print(functools.reduce(lambda a, b: a + b, [1, 2, 3, 4, 5]))

# 空序列 + 没有初始值：CPython 抛 TypeError
try:
    functools.reduce(add, [])
except TypeError:
    print("empty")

from functools import reduce, wraps

print(reduce(add, [1, 2]))


def traced(fn):
    @wraps(fn)
    def wrapper(a, b):
        return fn(a, b)

    return wrapper


@traced
def sub(a, b):
    return a - b


print(sub(10, 3))


@functools.wraps(add)
def plain(a, b):
    return a + b


print(plain(1, 2))

# ── 原 78_enumerate_zip.py ──────────────────────────────────────

# enumerate / zip 的 for 脱糖：直接计数循环，不建 [下标, 值] 的元组表
T = [10, 20, 30]
U = [1, 2]

for i, v in enumerate(T):
    print(i, v)

for a, b in zip(T, U):
    print(a, b)

for i, ch in enumerate("abc"):
    print(i, ch)

d = {"x": 1, "y": 2}
for i, k in enumerate(d):
    print(i, k)

for i, v in enumerate(T):
    if i == 1:
        continue
    print("skip", i)
else:
    print("done")

for a, b in zip(T, U):
    if a == 20:
        break
    print("pair", a, b)

# 空序列
for i, v in enumerate([]):
    print("never")
for a, b in zip([], T):
    print("never")

# 嵌套解构：退回物化路径，语义不变
pairs = [(1, 2), (3, 4)]
for idx, (x, y) in enumerate(pairs):
    print(idx, x, y)

# 当普通值用：仍然物化成列表（可 len、可下标）
e = enumerate([7, 8])
print(len(e), e[0][0], e[0][1])

# --- expect ---
# 10
# 16
# 24
# 18
# 4.0
# abc
# 15
# empty
# 3
# 7
# 3
# 0 10
# 1 20
# 2 30
# 10 1
# 20 2
# 0 a
# 1 b
# 2 c
# 0 x
# 1 y
# skip 0
# skip 2
# done
# pair 10 1
# 0 1 2
# 1 3 4
# 2 0 7
