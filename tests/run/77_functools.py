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
