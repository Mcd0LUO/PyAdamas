# 显式 object/Any 形参保持 unknown 域（0.326 F3）：float/int/str/bool/
# None/list 混用不报 mixed-float；默认值（含负数字面量）；方法/kwonly；
# 跨模块（argparse.add_argument 异构 default）；lambda 推断不受影响。
import argparse


def pick(x: object) -> object:
    return x


print(pick(3.5), pick(2), pick("s"), pick(True) == True)
v: object = pick(1.5)
print(v * 2)


def withdefault(x: object = None) -> object:
    return x


print(withdefault(1.5), withdefault(3), withdefault("a"))
print(withdefault() is None)


def negdefault(size: int = -1) -> int:
    return size


print(negdefault(), negdefault(5))


def fndefault(x: float = -2.5) -> float:
    return x


print(fndefault(), fndefault(1.0))


class Acc:
    last: object

    def __init__(self) -> None:
        self.last = None

    def put(self, v: object) -> None:
        self.last = v

    def get(self) -> object:
        return self.last


a = Acc()
a.put(2.5)
print(a.get())
a.put(7)
print(a.get())
a.put("s")
print(a.get())


def kw(*, val: object = 0) -> object:
    return val


print(kw(val=1.5), kw(val="x"), kw())


def apply(fn: object, x: object) -> object:
    return fn(x)


def double(n: int) -> int:
    return n * 2


print(apply(double, 21))

# lambda 推断仍按单态回填
f = lambda x: x + 1
print(f(1))
g = lambda x: x * 2.0
print(g(1.5))

# 跨模块：add_argument 的 default/type 异构
p = argparse.ArgumentParser(prog="t")
p.add_argument("--a", type=int, default=1)
p.add_argument("--b", type=float, default=2.5)
p.add_argument("--c", default="s")
p.add_argument("--d", action="store_true")
p.add_argument("--e", type=int, default=None)
ns = p.parse_args(["--b", "9.5"])
print(ns.a, ns.b, ns.c, ns.e is None)
print(ns.d == False)
# --- expect ---
# 3.5 2 s True
# 3.0
# 1.5 3 a
# True
# -1 5
# -2.5 1.0
# 2.5
# 7
# s
# 1.5 x 0
# 42
# 2
# 3.0
# 1 9.5 s True
# True
