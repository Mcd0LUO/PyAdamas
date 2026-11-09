# 生成器：yield / send / for / close / 跨 yield 清理
def count(n: int):
    i = 0
    while i < n:
        yield i
        i += 1


for x in count(3):
    print(x)

g = count(2)
print(next(g))
print(next(g))


def echo():
    a = yield 1
    yield a + 1


e = echo()
print(next(e))
print(e.send(41))


class Ctx:
    def __enter__(self) -> int:
        print("enter")
        return 5

    def __exit__(self, a, b, c) -> bool:
        print("exit")
        return False


def guarded():
    try:
        with Ctx() as v:
            yield v
            yield v + 1
    finally:
        print("finally")


it = guarded()
print(next(it))
print(next(it))
it.close()


def raiser():
    try:
        yield 1
        raise ValueError("boom")
    except ValueError:
        print("caught")
    yield 2


r = raiser()
print(next(r))
print(next(r))


def make(base: int):
    def gen():
        i = 0
        while i < 2:
            yield base + i
            i += 1

    return gen()


for v in make(10):
    print(v)


def abandoned():
    try:
        yield 1
    finally:
        print("abandoned cleanup")


def use_and_drop():
    a = abandoned()
    print(next(a))


use_and_drop()

# --- expect ---
# 0
# 1
# 2
# 0
# 1
# 1
# 42
# enter
# 5
# 6
# exit
# finally
# 1
# caught
# 2
# 10
# 11
# 1
# abandoned cleanup
