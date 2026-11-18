# 循环变量取模的语义回归（非负提示 / 作废 / 嵌套阴影）。
#
# 0.278 曾尝试「i % 常量 → 旋转余数槽」强度削减，实测负收益已撤
#（见 CHANGELOG 0.278：旋转是循环携带依赖，魔数链各轮独立可流水；
# 余数槽加剧寄存器池竞争）。本用例保留为取模语义回归：
#   1. 负 start / 变量 start（无符号魔数路径不得误用于负值）；
#   2. 多常量、c=1、2 的幂、大常量；
#   3. 体内重赋值作废非负提示（含赋非负值 i=7）；
#   4. break/continue；
#   5. 嵌套同名（内层负 range 阴影外层 —— 0.277 提示恢复洞的回归）、
#      嵌套异名、循环外使用；
#   6. 调用实参/下标/条件里的 mod、mod 的 mod、负除数/变量除数；
#   7. 生成器跨 yield、while/step2/try 内；
#   8. INT64_MAX 边界、10 万轮压力。

IMAX = 9223372036854775807

def basic(n: int) -> int:
    s = 0
    for i in range(n):
        s += i % 3 + i % 7 + i % 8 + i % 1 + i % 2 + i % 1000000007
    return s
print("basic", basic(100))

def negstart() -> int:
    s = 0
    for i in range(-10, 10):
        s += i % 3 + i % 7 + i % 8
    return s
print("negstart", negstart())

def varstart(a: int) -> int:
    s = 0
    for i in range(a, a + 20):
        s += i % 3 + i % 7
    return s
print("varstart", varstart(-7), varstart(0), varstart(5))

def reassign() -> int:
    s = 0
    for i in range(10):
        i = -5
        s += i % 3
    for i in range(10):
        i = 7
        s += i % 3
    for i in range(10):
        i += 1
        s += i % 3
    return s
print("reassign", reassign())

def brkcont() -> int:
    s = 0
    for i in range(20):
        if i % 2 == 0:
            continue
        if i > 10:
            break
        s += i % 3
    return s
print("brkcont", brkcont())

def nested() -> int:
    s = 0
    for i in range(3):
        for i in range(-4, 0):
            pass
        s += i % 3
    for i in range(3):
        for i in range(4):
            s += i % 3
        s += i % 3
    for a in range(4):
        for b in range(5):
            s += a % 3 + b % 3
    return s
print("nested", nested())

def after() -> int:
    for i in range(5):
        pass
    return i % 3
print("after", after())

def places() -> int:
    xs = [0, 1, 2, 3, 4]
    s = 0
    for i in range(20):
        s += abs(i % 3)
        s += xs[i % 5]
        if i % 4 == 1:
            s += 1
        s += (i % 5) % 3
        s += i // 3
        s += i % -3
        d = 3
        s += i % d
    return s
print("places", places())

def gen():
    for i in range(30):
        yield i % 7
print("gen", list(gen()))

def notreduced() -> int:
    s = 0
    i = 0
    while i < 10:
        s += i % 3
        i += 1
    for i in range(0, 20, 2):
        s += i % 3
    for i in range(10):
        try:
            s += i % 3
        except ZeroDivisionError:
            pass
    return s
print("notreduced", notreduced())

def imax() -> int:
    s = 0
    for i in range(IMAX - 6, IMAX):
        s += i % 3 + i % 7 + i % 8
    return s
print("imax", imax())

def stress() -> int:
    s = 0
    for i in range(100000):
        s += i % 3 + i % 7
    return s
print("stress", stress())
# --- expect ---
# basic 5736
# negstart 150
# varstart 77 76 79
# reassign 30
# brkcont 4
# nested 46
# after 1
# places 136
# gen [0, 1, 2, 3, 4, 5, 6, 0, 1, 2, 3, 4, 5, 6, 0, 1, 2, 3, 4, 5, 6, 0, 1, 2, 3, 4, 5, 6, 0, 1]
# notreduced 27
# imax 48
# stress 399994
