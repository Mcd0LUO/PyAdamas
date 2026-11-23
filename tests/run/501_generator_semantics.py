# 生成器语义回归（0.436）：yield / yield from / send / throw / close / finally、
# 异常传播、与 for/sum/list/zip 组合、交替消费、生命周期。
def g1():
    yield 1
    yield 2
    return 10


def g2():
    x = yield from g1()
    yield x
    yield 3


print("list", list(g1()))
print("sum", sum(g1()))
it = g1()
a = next(it)
b = next(it)
ok = 0
try:
    next(it)
except StopIteration:
    ok = 1
print("next", a, b, ok)
print("yieldfrom", list(g2()))
print("genexpr", [x for x in (y * 2 for y in range(3))])
def outer():
    for g in [g1(), g1()]:
        for v in g:
            yield v


print("nest", list(outer()))
def echo():
    got = []
    while True:
        x = yield len(got)
        if x is not None:
            got.append(x)
        if len(got) >= 2:
            return


e = echo()
v1 = next(e)
v2 = e.send(5)
ok2 = 0
try:
    e.send(6)
except StopIteration:
    ok2 = 1
print("send", v1, v2, ok2)
def catcher():
    try:
        yield 1
    except ValueError:
        yield 2


c = catcher()
print("throw_in", next(c))
print("throw_caught", c.throw(ValueError("x")))
def raiser():
    try:
        yield 1
    except KeyError:
        yield 2


r = raiser()
next(r)
bad = 0
try:
    r.throw(IndexError("nope"))
except IndexError:
    bad = 1
print("throw_uncaught", bad)
def boom():
    yield 1
    raise ValueError("gen-boom")


msg = 0
try:
    for v in boom():
        pass
except ValueError as err:
    msg = str(err)
print("propagate", msg)
def handled():
    try:
        yield 1
        raise KeyError("inner")
    except KeyError:
        yield "caught"


print("handled", list(handled()))
def fin_break():
    try:
        yield 1
        yield 2
        yield 3
    finally:
        print("fin_break")


out = []
for v in fin_break():
    out.append(v)
    if v == 2:
        break
print("break_out", out)
def nums(n: int):
    i = 0
    while i < n:
        yield i
        i = i + 1


print("zip", [(x, y) for x, y in zip(nums(3), nums(2))])
print("sum5", sum(nums(5)))
def outer_break():
    for x in nums(10):
        if x == 3:
            break
        yield x


print("outer_break", list(outer_break()))
def inter():
    a = nums(2)
    b = nums(2)
    out = []
    out.append(next(a))
    out.append(next(b))
    out.append(next(a))
    out.append(next(b))
    return out


print("interleave", inter())
fs = []
for i in range(3):
    def g() -> int:
        return i
    fs.append(g)
print("late_bind", [f() for f in fs])
def tracked():
    try:
        yield 1
        yield 2
    finally:
        print("tracked_fin")


lg = tracked()
next(lg)
del lg
print("after_del")
lg2 = tracked()
next(lg2)
lg2.close()
print("after_close")

# --- expect ---
# list [1, 2]
# sum 3
# next 1 2 1
# yieldfrom [1, 2, 10, 3]
# genexpr [0, 2, 4]
# nest [1, 2, 1, 2]
# send 0 1 1
# throw_in 1
# throw_caught 2
# throw_uncaught 1
# propagate gen-boom
# handled [1, 'caught']
# fin_break
# break_out [1, 2]
# zip [(0, 0), (1, 1)]
# sum5 10
# outer_break [0, 1, 2]
# interleave [0, 0, 1, 1]
# late_bind [2, 2, 2]
# tracked_fin
# after_del
# tracked_fin
# after_close
