# 只读借用槽细化（0.338）可挂起整类保守跳过：生成器跨挂起借用、
# 调用方在挂起期修改容器、genexpr、async/await。flag-on/off 行为一致。
import asyncio

def gen_borrow(xs: list[str]):
    for i in range(len(xs)):
        s = xs[i]
        yield s

def drive(xs: list[str]) -> int:
    total = 0
    for v in gen_borrow(xs):
        total += len(v)
        xs.append("g")
    return total

def genexpr_borrow(xs: list[str]) -> int:
    return sum(len(s) for s in xs)

def gen_finally(xs: list[str]):
    try:
        for i in range(len(xs)):
            s = xs[i]
            yield len(s)
    finally:
        yield -1

def drive_finally(xs: list[str]) -> int:
    total = 0
    for v in gen_finally(xs):
        total += v
    return total

def gen_send(xs: list[str]):
    for i in range(len(xs)):
        s = xs[i]
        x = yield s
        if x == 99:
            break

def drive_send(xs: list[str]) -> int:
    g = gen_send(xs)
    total = len(next(g))
    total += len(g.send(1))
    try:
        total += len(g.send(99))
    except StopIteration:
        total += 1000
    return total

async def aborrow(xs: list[str]) -> int:
    total = 0
    for i in range(len(xs)):
        s = xs[i]
        total += len(s)
        await asyncio.sleep(0)
    return total

def arun(xs: list[str]) -> int:
    return asyncio.run(aborrow(xs))

xs = ["alpha", "beta", "gamma"]
print(drive(xs))
print(genexpr_borrow(["a", "bb", "ccc"]))
print(drive_finally(["x", "yy"]))
print(drive_send(["p", "qq", "rrr"]))
print(arun(["q", "ww", "eee"]))

# --- expect ---
# 14
# 6
# 2
# 1003
# 6
