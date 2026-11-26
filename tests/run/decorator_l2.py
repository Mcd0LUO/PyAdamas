# 0.576：未知装饰器（L2）与 @lru_cache 的运行期语义
from functools import lru_cache

def deco(f):
    def wrapper(a: int, b: int) -> int:
        return f(a, b) + 10
    return wrapper

@deco
def add(a: int, b: int) -> int:
    return a + b

print(add(2, 3))

def twice(f):
    def wrapper(a: int, b: int) -> int:
        return f(a, b) * 2
    return wrapper

@deco
@twice
def mul(a: int, b: int) -> int:
    return a * b

print(mul(2, 3))

# lru_cache 真的缓存：函数体只执行 2 次（4 和 5 各一次）
calls = 0

@lru_cache
def slow(n: int) -> int:
    global calls
    calls = calls + 1
    return n * 2

print(slow(4), slow(4), slow(5), calls)

# --- expect ---
# 15
# 22
# 8 8 10 2
