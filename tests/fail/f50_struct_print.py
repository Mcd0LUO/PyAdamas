# expect: cannot print a struct value; print its fields instead
struct Point:
    x: float
    y: float

print(Point(1.0, 2.0))
