# 0.560：sizeof 接受 C 类型名（编译期常量），用于给 C 函数算字节数。
cimport clib as c

print(sizeof(int), sizeof(int64), sizeof(uint64))
print(sizeof(int32), sizeof(uint32), sizeof(float32))
print(sizeof(int16), sizeof(uint16), sizeof(int8), sizeof(uint8), sizeof(bool))
print(sizeof(float), sizeof(ptr), sizeof(ptr[float]))

xs: list[int] = [1, 2, 3]
ys: list[int] = [1, 2, 3]
print(c.memcmp(xs, ys, len(xs) * sizeof(int)))

struct Point:
    x: float
    y: float

p: Point = Point(1.0, 2.0)
print(sizeof(Point), sizeof(p))

# --- expect ---
# 8 8 8
# 4 4 4
# 2 2 1 1 1
# 8 8 8
# 0
# 16 16
