# 值类型 struct 的「只声明」局部变量（0.545）：声明处分配零填充 payload，
# 语义与标量一致（未初始化 = 零），之后写字段不再段错误。


struct Point:
    x: float
    y: float


def declared_then_written() -> float:
    p: Point
    p.x = 1.0
    p.y = 2.0
    return p.x + p.y


def declared_then_read() -> float:
    q: Point
    return q.x


print(declared_then_written())
print(declared_then_read())

# --- expect ---
# 3.0
# 0.0
