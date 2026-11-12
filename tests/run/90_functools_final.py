# functools 收尾：partial 的合成转发闭包与 lru_cache 的缓存包装
import functools
from functools import partial, lru_cache


def add(a: int, b: int) -> int:
    return a + b


p = functools.partial(add, 10)
print(p(5))
print(p(b=7))
q = partial(add, b=100)
print(q(3))
r = partial(add, 1, 2)
print(r())
p2 = partial(partial(add, 5), 6)
print(p2())


def greet(prefix: str, name: str) -> str:
    return prefix + name


g = partial(greet, "hi ")
print(g("bob"))


def scale(x: float, k: float) -> float:
    return x * k


s = partial(scale, 2.5)
print(s(4.0))


def greet2(greeting: str, *, name: str) -> str:
    return greeting + name


g2 = partial(greet2, "hey ")
print(g2(name="ann"))
g3 = partial(greet2, name="bob")
print(g3("yo "))

calls = 0


@lru_cache
def fib(n: int) -> int:
    global calls
    calls += 1
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


print(fib(10), calls)
print(fib(10), calls)


@functools.lru_cache(maxsize=None)
def sq(x: int) -> int:
    return x * x


print(sq(3), sq(3))


@functools.lru_cache
def two(a: int, b: int) -> int:
    return a * 10 + b


print(two(1, 2), two(1, 2))


@lru_cache
def tag(s: str) -> str:
    return "[" + s + "]"


print(tag("a"), tag("a"))


@lru_cache
def inc(x: int, d: int = 1) -> int:
    return x + d


print(inc(1), inc(1, 2), inc(1))


@functools.lru_cache(maxsize=2, typed=True)
def ident(x: int) -> int:
    return x


print(ident(1), ident(1))
# --- expect ---
# 15
# 17
# 103
# 3
# 11
# hi bob
# 10.0
# hey ann
# yo bob
# 55 11
# 55 11
# 9 9
# 12 12
# [a] [a]
# 2 3 2
# 1 1
