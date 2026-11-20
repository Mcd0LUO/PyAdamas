import random

def mk(xs: list[int]) -> tuple[int, ...]:
    return tuple(xs)

print(mk([1, 2, 3]))

class Point:
    x: int
    y: int

    def __init__(self, x: int, y: int) -> None:
        self.x = x
        self.y = y

    def total(self) -> int:
        return self.x + self.y

p = Point(1, 2)
print(p.total(), p.x)

class Bag:
    items: list[int]

    def __init__(self) -> None:
        self.items = []

    def add(self, v: int) -> None:
        self.items.append(v)

b = Bag()
b.add(3)
b.add(4)
print(b.items)

def gen(n: int):
    i = 0
    while i < n:
        yield i
        i += 1

for v in gen(3):
    print("g", v)

def deco(f):
    def wrapper(x: int) -> int:
        return f(x) * 2
    return wrapper

@deco
def twice(x: int) -> int:
    return x + 1

print(twice(5))

class Ctx:
    def __enter__(self) -> int:
        print("enter")
        return 7

    def __exit__(self, a, b, c) -> None:
        print("exit")

with Ctx() as v:
    print("body", v)

print(sorted([3, 1, 2], key=lambda x: -x))
d = {"a": 1}
print(d.get("b", 0), "a" in d)
print("-".join([str(x) for x in range(3)]))
print("%02x" % 15)
print(hex(255))
