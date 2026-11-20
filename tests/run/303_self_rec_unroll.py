# P3 纯标量自递归单层展开钉子（PYADAMAS_INLINE_OPT）：fib/fact/双参尾递归
# 形态展开后值必须逐位一致；负例（heap 实参递归、含 handler 递归、自调用
# 点 >2 的 ackermann）不得展开且语义不变；深递归仍正确。
def fib(n: int) -> int:
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


def fact(n: int) -> int:
    if n <= 1:
        return 1
    return n * fact(n - 1)


def sum_down(n: int, acc: int) -> int:
    if n == 0:
        return acc
    return sum_down(n - 1, acc + n)


def ack(m: int, n: int) -> int:
    if m == 0:
        return n + 1
    if n == 0:
        return ack(m - 1, 1)
    return ack(m - 1, ack(m, n - 1))


print(fib(0), fib(1), fib(2), fib(10), fib(20))
print(fact(0), fact(1), fact(15))
print(sum_down(500, 0))
print(ack(2, 3))


def list_sum(xs: list[int], i: int) -> int:
    if i >= len(xs):
        return 0
    return xs[i] + list_sum(xs, i + 1)


print(list_sum([1, 2, 3, 4, 5], 0))


def safe_rec(n: int) -> int:
    try:
        if n == 0:
            raise ValueError("x")
        return safe_rec(n - 1)
    except ValueError:
        return -1


print(safe_rec(10))


def deep(n: int) -> int:
    if n == 0:
        return 0
    return deep(n - 1) + 1


print(deep(500))

# --- expect ---
# 0 1 1 55 6765
# 1 1 1307674368000
# 125250
# 9
# 15
# -1
# 500
