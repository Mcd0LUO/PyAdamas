# 跨模块类写进形参/返回注解（import mod 与 from mod import Cls 两种写法）。
import shapes
from shapes import Point


def shift(p: shapes.Point, dx: int, dy: int) -> shapes.Point:
    return Point(p.x + dx, p.y + dy)


def total(p: Point) -> int:
    return p.x + p.y


def make(dx: int) -> Point:
    return shift(Point(1, 2), dx, dx)


q = shift(shapes.Point(1, 2), 10, 20)
print(q.x, q.y)
print(total(q))
print(total(make(3)))
print(q.total())

o = shapes.origin()
print(o.x, o.y)
print(o.total())

# --- expect ---
# 11 22
# 33
# 9
# 33
# 0 0
# 0
