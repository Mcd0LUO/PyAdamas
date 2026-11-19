# 类型名当值（0.326 F4）：int/float/str 等是类型对象，同一性/相等、
# print/str、容器装载、isinstance/issubclass、type(x)、跨模块分发。
import argparse

a = int
b = float
print(a is int, a is float, a == int, a == float)
print(b is int, b is float)
c: object = int
print(c is int, c is float)
print(type(5) is int, type(2.5) is float, type("s") is str)
print(type(True) is bool, type([1]) is list, type({}) is dict)
print(int, float, str)
print(str(int), str(float))
ts: list[object] = [int, str, float]
print(ts[0] is int, ts[1] is str, ts[2] is float)
print(int in ts, bool in ts)


print(isinstance(5, int), isinstance("s", str), isinstance(2.5, float))
print(isinstance(5, (str, int)), isinstance("s", (str, int)))
print(issubclass(bool, int), issubclass(int, object), issubclass(str, int))

# 类型对象当构造器按名字直调仍可用
print(int("42"), float("2.5"), str(7), bool(0))


# 跨模块分发（argparse type=）
def mytype(s: str) -> str:
    return s.upper()


p = argparse.ArgumentParser(prog="t")
p.add_argument("--i", type=int)
p.add_argument("--f", type=float)
p.add_argument("--s", type=str)
p.add_argument("--u", type=mytype)
ns = p.parse_args(["--i", "42", "--f", "2.5", "--s", "hi", "--u", "abc"])
print(ns.i + 1, ns.f * 2, ns.s, ns.u)
# --- expect ---
# True False True False
# False True
# True False
# True True True
# True True True
# <class 'int'> <class 'float'> <class 'str'>
# <class 'int'> <class 'float'>
# True True True
# True False
# True True True
# True True
# True True False
# 42 2.5 7 False
# 43 5.0 hi ABC
