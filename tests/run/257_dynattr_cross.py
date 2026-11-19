# 跨模块动态属性（0.326 F1/F2）：导入类的实例直读/直写/getattr/setattr/
# hasattr/vars；float 经非字面量 setattr 装箱；unknown 列表迭代/list()。
# 同模块动态属性 + 多实例隔离 + 覆盖写。
import argparse


class Box:
    def __init__(self) -> None:
        self.tag: int = 0

    def put(self, name: str, value: object) -> None:
        setattr(self, name, value)


b = Box()
b.put("i", 5)
b.put("f", 2.5)
b.put("s", "hi")
b.put("lst", ["a", "b", "c"])
b.put("d", {"k": 9})
print(b.i, b.i + 1)
print(b.f * 2.0)
print(b.s + "!")
print(len(b.lst), b.lst[1])
for x in b.lst:
    print(x)
dd: dict[str, int] = b.d
print(dd["k"])
print(getattr(b, "i"), getattr(b, "nope", "dflt"))
print(hasattr(b, "i"), hasattr(b, "nope"))
b.extra = 7
print(b.extra)
b.extra = 8
print(b.extra)
d = vars(b)
print(d["i"], d["f"], d["s"], d["extra"], d["tag"])
print(len(d))
b2 = Box()
print(hasattr(b2, "i"))
b2.put("i", 100)
print(b2.i, b.i)

# 跨模块：argparse.Namespace（定义在 stdlib/argparse.py）
ns = argparse.Namespace()
setattr(ns, "x", 5)
setattr(ns, "fl", 1.5)
setattr(ns, "st", "v")
print(ns.x, ns.fl * 2, ns.st)
ns.y = 9
print(ns.y, getattr(ns, "y"), hasattr(ns, "y"))
print(vars(ns)["x"])
key = "dyn"
setattr(ns, key, 2.25)
print(ns.dyn + 0.25)
lst: list[object] = [1, "two", 3.5]
setattr(ns, "mix", lst)
for v in ns.mix:
    print(v)
print(len(ns.mix))
copied = list(ns.mix)
print(copied[0], copied[1], copied[2])

# parse_args 结果上的动态属性（type=float 默认值装箱）
p = argparse.ArgumentParser(prog="t")
p.add_argument("--rate", type=float, default=0.5)
p.add_argument("--name", default="n")
p.add_argument("--count", type=int, default=2)
a = p.parse_args([])
print(a.rate * 4, a.name, a.count + 1)
a.rate = 2.5
print(a.rate)
# --- expect ---
# 5 6
# 5.0
# hi!
# 3 b
# a
# b
# c
# 9
# 5 dflt
# True False
# 7
# 8
# 5 2.5 hi 8 0
# 7
# False
# 100 5
# 5 3.0 v
# 9 9 True
# 5
# 2.5
# 1
# two
# 3.5
# 3
# 1 two 3.5
# 2.0 n 3
# 2.5
