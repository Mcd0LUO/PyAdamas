# yield from：委托生成器 / 序列 / send 转发 / close / 异常传播 / 返回值


def inner(n: int):
    try:
        i = 0
        while i < n:
            yield i
            i += 1
    finally:
        print("inner finally")


def outer():
    x = yield from inner(2)
    if x is None:
        print("delegate returned none")
    yield 9


for v in outer():
    print(v)


def echo():
    while True:
        v = yield
        if v is None:
            continue
        print("echo", v)


def fwd():
    yield from echo()


g = fwd()
next(g)
g.send(10)
g.send(20)
g.close()
print("forwarder closed")


def nums():
    yield from [1, 2]
    yield from (3, 4)


for v in nums():
    print(v)


def chars():
    yield from "z"


for c in chars():
    print(c)


def keys():
    yield from {"a": 1, "b": 2}


for k in keys():
    print(k)


def sub():
    try:
        yield 1
        yield 2
    finally:
        print("sub finally")


def wrap():
    yield from sub()
    yield 3


w = wrap()
print(next(w))
w.close()
print("wrapped closed")


def bad():
    yield 1
    raise ValueError("boom")


def relay():
    yield from bad()


try:
    for v in relay():
        print(v)
except ValueError as e:
    print("caught", e)


def layer1():
    yield from nums()
    yield 5


def layer2():
    yield from layer1()


for v in layer2():
    print(v)


def ret_sub():
    yield 1
    return 7


def ret_outer():
    r = yield from ret_sub()
    print("ret", r)
    yield 8


for v in ret_outer():
    print(v)

# --- expect ---
# 0
# 1
# inner finally
# delegate returned none
# 9
# echo 10
# echo 20
# forwarder closed
# 1
# 2
# 3
# 4
# z
# a
# b
# 1
# sub finally
# wrapped closed
# 1
# caught boom
# 1
# 2
# 3
# 4
# 5
# 1
# ret 7
# 8
