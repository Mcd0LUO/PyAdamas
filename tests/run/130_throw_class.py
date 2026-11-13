# 生成器 throw() / 异步生成器 athrow() 接受异常类，等价于无消息实例。

import asyncio


class MyError(Exception):
    pass


def g():
    try:
        yield 1
    except ValueError:
        print("g caught")
        yield 2


it = g()
print("first", next(it))
v = it.throw(ValueError)
print("threw", v)
try:
    next(it)
except StopIteration:
    print("done")


def c():
    try:
        yield 10
    except MyError:
        print("c caught")
        yield 20


ic = c()
print("c first", next(ic))
v = ic.throw(MyError)
print("c threw", v)
try:
    next(ic)
except StopIteration:
    print("c done")


# 未启动 / 已结束：异常直接抛给调用方。
def h():
    yield 100


u = h()
try:
    u.throw(ValueError)
except ValueError:
    print("unstarted")
try:
    next(u)
except StopIteration:
    print("unstarted stop")

f = h()
print("f first", next(f))
try:
    next(f)
except StopIteration:
    print("f stop")
try:
    f.throw(ValueError)
except ValueError:
    print("finished")


async def ag():
    try:
        yield 1
    except ValueError:
        print("ag caught")
        yield 2


async def main():
    a = ag()
    print("ag first", await a.__anext__())
    v = await a.athrow(ValueError)
    print("ag threw", v)
    try:
        await a.__anext__()
    except StopAsyncIteration:
        print("ag done")


asyncio.run(main())

# --- expect ---
# first 1
# g caught
# threw 2
# done
# c first 10
# c caught
# c threw 20
# c done
# unstarted
# unstarted stop
# f first 100
# f stop
# finished
# ag first 1
# ag caught
# ag threw 2
# ag done
