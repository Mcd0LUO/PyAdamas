# addr(struct) 直接交给 C：C 看到的就是标准结构体布局（两个 double）。
cimport clib as c

struct Point:
    x: float
    y: float

p = Point(1.0, 2.0)
print(sizeof(Point))
c.memset(addr(p), 0, 16)
print(p.x)
print(p.y)
p.x = 5.0
c.memset(addr(p), 0, 8)
print(p.x)

# --- expect ---
# 16
# 0.0
# 0.0
# 0.0
