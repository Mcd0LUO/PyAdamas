# 520_alloc_churn.py —— 分配/释放路径 churn（分配器调试开关的常规回归）
#
# 覆盖 runtime 分配器的各类对象与释放路径：短/长 str（SSO 与堆串）、
# list（内联/增长）、dict、tuple、类实例、闭包、异常、生成器、big 字面量
# 与 JSON 盒。默认运行时是普通 malloc/free；PYADAMAS_CANARY=1 /
# PYADAMAS_GUARD=1 时走同一份源码的调试版运行时（尾部 canary / guard
# page），本用例用来确认那条路径没有误报、且对象内容都正确。
import json


class Point:
    def __init__(self, x: int, y: int) -> None:
        self.x = x
        self.y = y


def make_closure(n: int):
    def inner(k: int) -> int:
        return n + k
    return inner


def gen_sums(n: int):
    i = 0
    while i < n:
        yield i * 2
        i += 1


total = 0
for i in range(120):
    short = "s" * (i % 7)          # SSO 短串
    long_s = "L" * (i % 40 + 8)    # 堆串（>7 字节）
    xs = [i, i + 1, short]
    xs.append(long_s)              # 触发容量增长
    d = {"n": i, "xs": xs, "s": long_s}
    t = (i, short, long_s)
    p = Point(i, i * 2)
    f = make_closure(i)
    try:
        raise ValueError("boom")
    except ValueError:
        pass
    g = gen_sums(4)
    big = 10 ** 30 if i % 10 == 0 else 0
    j = json.parse('{"a": [1, true, null], "b": "x"}')
    total += p.x + p.y + f(3) + len(long_s) + len(t[2])
    for v in g:
        total += v
    total += len(json.stringify(j))
    if i % 40 == 0:
        print("churn", i, total, short == "s" * (i % 7), d["n"] == i, big > 0)

print("done", total)
print(json.stringify([1.5, 2.0]))
print(json.stringify({"ok": json.bool(True), "n": 3, "extra": json.null()}))

# --- expect ---
# churn 0 63 True True True
# churn 40 7423 True True True
# churn 80 21183 True True True
# done 40800
# [1.5, 2.0]
# {"ok": true, "n": 3, "extra": null}
