# 类名当值：运行期类对象、构造、isinstance、__name__ 与打印。

class Point:
    x: int
    y: int

    def __init__(self, x: int, y: int = 0) -> None:
        self.x = x
        self.y = y

    def norm2(self) -> int:
        return self.x * self.x + self.y * self.y


f = Point
p = f(3, 4)
print(p.norm2())
print(isinstance(p, Point))
print(isinstance(p, f))
print(f.__name__)
print(Point.__name__)
print(f)
print(str(f))
print(repr(f))


def build(cls, x: int) -> Point:
    return cls(x)


q = build(Point, 5)
print(q.x)
print(issubclass(Point, Point))

# --- expect ---
# 25
# True
# True
# Point
# Point
# <class 'Point'>
# <class 'Point'>
# <class 'Point'>
# 5
# True
