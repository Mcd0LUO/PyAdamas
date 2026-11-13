import shapes

p = shapes.Point(3, 4)
print(p.norm2())
print(p.z)
p.move(1, 1)
print(p.x, p.y)

from shapes import Point
q = Point(1)
print(q.x, q.y, q.z)
q.z = 7
print(q.norm2() + q.z)
import shapes

p = shapes.Point(x=3, y=4)
print(p.norm2())

c = shapes.Child()
print(c.n, c.label(), c.total(10))
d = shapes.Child(n=7, tag="d")
print(d.n, d.label(), d.total(1))

from shapes import Child
e = Child(9, "e")
print(e.label(), e.n)

# 跨模块函数返回类实例：接口按名字序列化类引用
r = shapes.make_point(5, 12)
print(r.norm2())

# --- expect ---
# 25
# 5
# 4 5
# 1 0 5
# 8
# 25
# 2 child 12
# 7 d 8
# e 9
# 169
