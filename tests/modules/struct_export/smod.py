struct Point:
    x: float
    y: float

def make(a: float, b: float) -> Point:
    p: Point = Point(0.0, 0.0)
    p.x = a
    p.y = b
    return p

def norm2(p: Point) -> float:
    return p.x * p.x + p.y * p.y
