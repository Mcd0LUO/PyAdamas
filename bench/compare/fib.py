# fib —— 朴素递归斐波那契（函数调用开销）
N = 32


def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


print(fib(N))
