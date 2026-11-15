# functools 的 stdlib 扩展：原生模块 + stdlib/functools.py 补口。
# 这些名字原生 functools 没有——旧的硬编码内建名单下同名的 stdlib 源
# 会被内建名单挡住，根本加载不进来。
import functools
from functools import cache, update_wrapper, WRAPPER_ASSIGNMENTS, WRAPPER_UPDATES


@cache
def fib(n: int) -> int:
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


print(fib(10))


def inc(n: int) -> int:
    return n + 1


g = update_wrapper(inc, inc)
print(g(41))

print(len(WRAPPER_ASSIGNMENTS) > 0)
print(WRAPPER_UPDATES[0])

# 模块属性形式的扩展全局：值读取与调用都要走通。
print(functools.update_wrapper(inc, inc)(1))
print(len(functools.WRAPPER_ASSIGNMENTS))

# 原生名字不受影响（原生优先，仍然走编译器内建降级）。
print(functools.reduce(lambda a, b: a + b, [1, 2, 3, 4]))
# --- expect ---
# 55
# 42
# True
# __dict__
# 2
# 5
# 10
