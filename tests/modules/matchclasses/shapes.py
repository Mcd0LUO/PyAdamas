class Shape:
    def kind(self) -> str:
        return "shape"


class Circle(Shape):
    def kind(self) -> str:
        return "circle"


class Point:
    __match_args__ = ("x", "y")

    def __init__(self, x: int, y: int) -> None:
        self.x = x
        self.y = y
