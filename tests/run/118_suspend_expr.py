# 挂起点泛化：yield/await 可以出现在任意子表达式里。
import asyncio


def three(a: int, b: int, c: int) -> int:
    return a * 100 + b * 10 + c


class Box:
    def __init__(self, v: int) -> None:
        self.v = v

    def add(self, a: int, b: int) -> int:
        return self.v + a + b


def gen():
    b = Box(1000)
    r1 = three(1, (yield 1), 3)
    print("three", r1)
    r2 = b.add((yield 2), 5)
    print("method", r2)
    xs = [7, 8, 9]
    i = 1
    r3 = xs[i] + (yield 3)
    print("sub", r3)
    r4 = (yield 4) < 5 < 6
    print("cmp", r4)
    r5 = "n" + f"-{(yield 5)}-"
    print("fstr", r5)
    p = (yield 6)
    print("p", p, "q")
    r6 = str(11) + str((yield 7)) + str(22)
    print("cat", r6)
    left = (yield 8) + 0
    r7 = left + (yield 9)
    print("twice", r7)
    print("end")


g = gen()
v = next(g)
print("got", v)
v = g.send(11)
print("got", v)
v = g.send(12)
print("got", v)
v = g.send(13)
print("got", v)
v = g.send(14)
print("got", v)
v = g.send(15)
print("got", v)
v = g.send(16)
print("got", v)
v = g.send(16)
print("got", v)
v = g.send(18)
print("got", v)
try:
    v = g.send(19)
    print("got", v)
except StopIteration:
    print("stop")


def closing():
    try:
        v = "pre" + (yield 1)
        print(v)
    finally:
        print("cleanup")


c = closing()
v = next(c)
print("close", v)
c.close()


async def add(a: int, b: int) -> int:
    return a + b


async def main() -> None:
    x = await add(1, (await add(2, 3)))
    print("await1", x)
    y = (await add(10, 20)) + 5
    print("await2", y)
    r = await add(await add(1, 2), await add(3, 4))
    print("await3", r)
    s = ""
    i = 0
    while i < 3:
        s = s + str((await add(i, 100))) + ","
        i += 1
    print("await4", s)


asyncio.run(main())

# --- expect ---
# got 1
# three 213
# got 2
# method 1017
# got 3
# sub 21
# got 4
# cmp False
# got 5
# fstr n-15-
# got 6
# p 16 q
# got 7
# cat 111622
# got 8
# got 9
# twice 37
# end
# stop
# close 1
# cleanup
# await1 6
# await2 35
# await3 10
# await4 100,101,102,
