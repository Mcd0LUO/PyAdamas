import math


class Point:
    x: int
    y: int
    __match_args__ = ("x", "y")

    def __init__(self, x: int, y: int) -> None:
        self.x = x
        self.y = y


class Point3(Point):
    z: int
    __match_args__ = ("x", "y", "z")

    def __init__(self, x: int, y: int, z: int) -> None:
        self.x = x
        self.y = y
        self.z = z


def describe(p: Point) -> str:
    match p:
        case Point(0, 0):
            return "origin"
        case Point(x=0, y=y):
            return "y-axis"
        case Point(x, 0):
            return "x-axis"
        case Point(x, y) if x == y:
            return "diag"
        case Point(x, y):
            return "point"
        case _:
            return "?"


print(describe(Point(0, 0)))
print(describe(Point(0, 5)))
print(describe(Point(3, 0)))
print(describe(Point(2, 2)))
print(describe(Point(2, 3)))
print(describe(Point3(0, 0, 9)))


def narrow(p: Point) -> str:
    match p:
        case Point3(x, y, z):
            return "p3"
        case _:
            return "base"


print(narrow(Point3(1, 2, 3)))
print(narrow(Point(1, 2)))


def dynamic(v) -> str:
    match v:
        case Point(0, 0):
            return "origin"
        case [a, b]:
            return "pair"
        case {"k": x}:
            return "map"
        case _:
            return "other"


print(dynamic(Point(0, 0)))
print(dynamic(Point(1, 2)))
print(dynamic([4, 5]))
print(dynamic({"k": 6}))
print(dynamic(7))


def value_pattern(x: float) -> str:
    match x:
        case math.pi:
            return "pi"
        case _:
            return "other"


print(value_pattern(math.pi))
print(value_pattern(3.0))


def as_pattern(xs: list[int]) -> str:
    match xs:
        case [a, b] as pair:
            return str(len(pair)) + ":" + str(a + b)
        case _:
            return "no"


print(as_pattern([1, 2]))
print(as_pattern([1]))


def or_capture(v) -> str:
    match v:
        case [x] | {"k": x}:
            return "one"
        case _:
            return "no"


print(or_capture([9]))
print(or_capture({"k": 1}))
print(or_capture("x"))


def rest(d: dict[str, int]) -> str:
    match d:
        case {"a": v, "b": w, **extra}:
            return str(v + w) + ":" + str(len(extra))
        case {"a": v, **extra}:
            return "a" + str(len(extra))
        case _:
            return "?"


print(rest({"a": 1, "b": 2}))
print(rest({"a": 1, "b": 2, "c": 3}))
print(rest({"a": 1, "x": 2}))
print(rest({"b": 2}))


def exc_pattern(flag: bool) -> str:
    try:
        if flag:
            raise ValueError("bad")
        raise KeyError("k")
    except Exception as e:
        match e:
            case ValueError():
                return "value"
            case KeyError():
                return "key"
            case _:
                return "other"


print(exc_pattern(True))
print(exc_pattern(False))


def builtin_pat(x: int) -> str:
    match x:
        case int():
            return "int"
        case _:
            return "no"


def builtin_float(x: float) -> str:
    match x:
        case int():
            return "int"
        case float():
            return "float"
        case _:
            return "no"


print(builtin_pat(5))
print(builtin_float(1.5))


def dup(xs: list[int]) -> str:
    match xs:
        case [a, a]:
            return "eq"
        case _:
            return "no"


print(dup([2, 2]))
print(dup([2, 3]))

# --- expect ---
# origin
# y-axis
# x-axis
# diag
# point
# origin
# p3
# base
# origin
# other
# pair
# map
# other
# pi
# other
# 2:3
# no
# one
# one
# no
# 3:0
# 3:1
# a1
# ?
# value
# key
# int
# float
# eq
# no
