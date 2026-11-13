# 生成器 throw()：在挂起点注入异常；yield from 把异常转发给子生成器，
# 子生成器结束时 yield from 返回它的 return 值。
# 注意：先算值再打印，避免 print 逐项输出与生成器内部打印的顺序差异。


def g():
    try:
        v = yield 1
        print("g got", v)
    except ValueError as e:
        print("g caught", e)
    yield 2


it = g()
v = next(it)
print("first", v)
v = it.throw(ValueError("boom"))
print("threw", v)
try:
    next(it)
except StopIteration:
    print("done")


# 未捕获：异常传向调用方，生成器随之结束。
def h():
    yield 10
    print("h never")


i2 = h()
v = next(i2)
print("h first", v)
try:
    i2.throw(KeyError("k"))
except KeyError:
    print("escaped")
try:
    next(i2)
except StopIteration:
    print("h stop")


# 已经结束的生成器：异常直接抛给调用方。
def empty():
    if False:
        yield 1


i3 = empty()
try:
    next(i3)
except StopIteration:
    print("empty stop")
try:
    i3.throw(RuntimeError("late"))
except RuntimeError as e:
    print("late", e)


# 从未启动的生成器：函数体不执行，异常从入口抛出。
def never():
    print("never body")
    yield 1


i4 = never()
try:
    i4.throw(TypeError("early"))
except TypeError as e:
    print("early", e)
try:
    next(i4)
except StopIteration:
    print("never stop")


# yield from：子生成器接住异常后继续 yield。
def sub():
    try:
        x = yield "s1"
        print("sub got", x)
        y = yield "s1b"
        print("sub got", y)
    except ValueError as e:
        print("sub caught", e)
        x = yield "s2"
        print("sub got", x)
    return "sub-return"


def outer():
    r = yield from sub()
    print("outer result", r)
    yield "o1"


o = outer()
v = next(o)
print("o", v)
v = o.send("x")
print("o", v)
v = o.throw(ValueError("v"))
print("o", v)
v = o.send("y")
print("o", v)
try:
    next(o)
except StopIteration:
    print("o stop")


# yield from：子生成器没接住，异常在外层挂起点重新抛出。
def sub2():
    yield 1
    yield 2


def outer2():
    try:
        yield from sub2()
    except KeyError:
        print("outer2 caught")
        yield "recovered"


p = outer2()
v = next(p)
print("p", v)
v = p.throw(KeyError("kk"))
print("p", v)
try:
    next(p)
except StopIteration:
    print("p stop")


# yield from：子生成器被 throw 后结束，yield from 的值是它的 return 值。
def sub3():
    try:
        yield 1
    except ValueError:
        return "recovered-value"


def outer3():
    r = yield from sub3()
    print("outer3 result", r)
    yield 9


q = outer3()
v = next(q)
print("q", v)
v = q.throw(ValueError("x"))
print("q", v)
try:
    next(q)
except StopIteration:
    print("q stop")


# throw 之后仍能 close：finally 在异常传播时跑，生成器标记为结束。
def g5():
    try:
        yield 1
        yield 2
    finally:
        print("g5 finally")


i5 = g5()
v = next(i5)
print("g5", v)
try:
    i5.throw(KeyError("z"))
except KeyError:
    print("g5 escaped")
i5.close()
print("g5 closed")

# --- expect ---
# first 1
# g caught boom
# threw 2
# done
# h first 10
# escaped
# h stop
# empty stop
# late late
# early early
# never stop
# o s1
# sub got x
# o s1b
# sub caught v
# o s2
# sub got y
# outer result sub-return
# o o1
# o stop
# p 1
# outer2 caught
# p recovered
# p stop
# q 1
# outer3 result recovered-value
# q 9
# q stop
# g5 1
# g5 finally
# g5 escaped
# g5 closed
