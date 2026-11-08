# 寄存器分配：变量提升、调用约定、栈槽布局
#
# 这一组钉住的是后端最容易错、错了又最难查的东西：
#   1. callee-saved 寄存器的保存槽不能被局部变量覆盖
#   2. 超过 5 个局部变量时，多出来的正确退回栈槽
#   3. 超过 6 个整型参数时，多出来的从栈上传
#   4. 循环里同时活跃的变量跨基本块仍然正确

def callee(n: int) -> int:
    # 这些变量会被提升到 callee-saved 寄存器
    a = n
    b = a * 2
    c = b + 1
    d = c * 3
    e = d - 4
    return a + b + c + d + e


def caller() -> int:
    # 调用方自己也占着几个寄存器，callee 必须原样还回来
    x = 7
    y = 11
    z = 13
    s = callee(5)
    return x + y + z + s


print("caller", caller())
print("callee", callee(1))


def many(a: int) -> int:
    # 12 个局部变量，只有最热的 5 个能进寄存器
    v1 = a
    v2 = v1 + 1
    v3 = v2 + 1
    v4 = v3 + 1
    v5 = v4 + 1
    v6 = v5 + 1
    v7 = v6 + 1
    i = 0
    total = 0
    while i < a:
        total = total + v1 + v2 + v3 + v4 + v5 + v6 + v7
        i = i + 1
    return total + v3 * 2


print("many", many(9))


def eight(a: int, b: int, c: int, d: int, e: int, f: int, g: int, h: int) -> int:
    # 8 个参数：前 6 个走寄存器，后 2 个从栈上读
    return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8


print("eight", eight(1, 2, 3, 4, 5, 6, 7, 8))


def compare(a: int, b: int) -> int:
    # 六种比较都要能直接落成条件跳转
    n = 0
    if a < b:
        n += 1
    if a <= b:
        n += 10
    if a > b:
        n += 100
    if a >= b:
        n += 1000
    if a == b:
        n += 10000
    if a != b:
        n += 100000
    return n


print("cmp", compare(3, 5), compare(5, 5), compare(9, 2))


def nested(n: int) -> int:
    # 双层循环，两个循环变量 + 累加器都要跨块活着
    total = 0
    i = 0
    while i < n:
        j = 0
        while j < n:
            total += i * j
            j += 1
        i += 1
    return total


print("nested", nested(6))


def fib(n: int) -> int:
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


print("fib", fib(15))

# --- expect ---
# caller 119
# callee 20
# many 778
# eight 204
# cmp 100011 11010 101100
# nested 225
# fib 610
