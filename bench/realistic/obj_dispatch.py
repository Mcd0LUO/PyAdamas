# obj_dispatch —— 类继承 + 虚方法分派（OOP 密集型）
N = 640000

class Shape:
    def area(self) -> int:
        return 0
    def name(self) -> str:
        return "shape"

class Rect(Shape):
    def __init__(self, w: int, h: int) -> None:
        self.w = w
        self.h = h
    def area(self) -> int:
        return self.w * self.h
    def name(self) -> str:
        return "rect"

class Square(Rect):
    def __init__(self, s: int) -> None:
        self.w = s
        self.h = s
    def name(self) -> str:
        return "square"

shapes = []
for i in range(100):
    if i % 3 == 0:
        shapes.append(Shape())
    elif i % 3 == 1:
        shapes.append(Rect(i, i + 1))
    else:
        shapes.append(Square(i))

total = 0
for r in range(N):
    for s in shapes:
        total += s.area()
print(total)
