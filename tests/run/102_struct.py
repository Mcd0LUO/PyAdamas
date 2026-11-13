# 值类型 struct：C 兼容布局、字段读写、构造、sizeof、addr 与值语义。
struct Point:
    x: float
    y: float

struct Flags:
    on: bool
    code: int

p = Point(1.0, 2.0)
print(p.x)
print(p.y)
print(sizeof(Point))
print(sizeof(p))

# 值语义：q 拿到的是独立副本
q = p
q.x = 9.0
print(p.x)
print(q.x)

# 传参和返回也按值复制
def shift(v: Point, dx: float) -> Point:
    v.x = v.x + dx
    return v

r = shift(p, 1.5)
print(p.x)
print(r.x)

# addr 得到 ptr[Point]，字段读写自动解引用
a = addr(p)
a.y = 7.0
print(p.y)

# bool 字段只占 1 字节，后面补齐到 8 再放 int
print(sizeof(Flags))
f = Flags(True, 3)
print(f.on)
print(f.code)
f.on = False
print(f.on)

# 函数内的局部 struct 与复合赋值
def make() -> Point:
    t = Point(3.0, 4.0)
    t.x += 1.0
    return t

m = make()
print(m.x)
print(m.y)

p.x += 0.5
print(p.x)

# --- expect ---
# 1.0
# 2.0
# 16
# 16
# 1.0
# 9.0
# 1.0
# 2.5
# 7.0
# 16
# True
# 3
# False
# 4.0
# 4.0
# 1.5
