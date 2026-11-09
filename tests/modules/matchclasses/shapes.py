class Shape:
    def kind(self) -> str:
        return "shape"


class Circle(Shape):
    def kind(self) -> str:
        return "circle"
