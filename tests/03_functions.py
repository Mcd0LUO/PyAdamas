# 函数：标注、默认参数、可变参数、递归、lambda、装饰器语法
def fib(n: int) -> int:
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


def greet(name: str, greeting: str = "hello", punct: str = "!") -> str:
    return f"{greeting}, {name}{punct}"


def variadic(first: int, *rest, sep: str = "-", **kwargs) -> str:
    parts = [str(first)]
    for r in rest:
        parts.append(str(r))
    return sep.join(parts)


def outer(x: int):
    def inner(y: int) -> int:
        return x + y
    return inner


double = lambda v: v * 2
add = lambda a, b=1: a + b

squares = [v * v for v in range(10) if v % 2 == 0]
pairs = {k: v for k, v in zip("abc", [1, 2, 3])}
uniq = {v % 3 for v in range(10)}
gen = (v for v in range(3))


@staticmethod
def decorated() -> None:
    pass


class Point:
    pass
