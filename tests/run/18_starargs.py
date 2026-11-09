# *args / **kwargs：定义、调用打包、展开、方法、闭包与转发
def f0(*args: int) -> int:
    total: int = 0
    for x in args:
        total = total + x
    return total

def g(a: int, *rest: int) -> int:
    return a + len(rest)

def h(**kw: int) -> int:
    return kw["a"] + kw["b"]

def m(a: int, **kw: int) -> int:
    return a + kw["z"]

def fwd(**kw: int) -> int:
    return h(**kw)

class Box:
    def __init__(self, *items: str) -> None:
        self.n = len(items)
        self.first = items[0] if len(items) > 0 else ""

    def count(self, *extra: int) -> int:
        return self.n + len(extra)

def outer(*args: int) -> int:
    def inner() -> int:
        return len(args)
    return inner()

print(f0())
print(f0(1, 2, 3))
lst: list[int] = [4, 5, 6]
print(f0(*lst))
t: tuple[int, int, int] = (7, 8, 9)
print(f0(*t))
print(g(1))
print(g(1, 2, 3))
print(g(*t))
print(g(1, *lst))
print(h(a=1, b=2))
d: dict[str, int] = {"a": 30, "b": 40}
print(h(**d))
print(m(1, z=5))
print(fwd(a=2, b=3))

b: Box = Box("a", "b", "c")
print(b.n)
print(b.first)
print(b.count())
print(b.count(1, 2))
print(outer())
print(outer(1, 2, 3))

size = lambda *xs: len(xs)
print(size(1, 2, 3, 4))

def joinish(**kw: str) -> str:
    return kw["a"] + kw["b"]

print(joinish(a="x", b="y"))
sd: dict[str, str] = {"a": "p", "b": "q"}
print(joinish(**sd))
# --- expect ---
# 0
# 6
# 15
# 24
# 1
# 3
# 9
# 4
# 3
# 70
# 6
# 5
# 3
# a
# 3
# 5
# 0
# 3
# 4
# xy
# pq
