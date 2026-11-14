# 跨模块 super()：Derived 的基类来自另一个模块，super().__init__ 和
# super().describe() 都要沿运行期 MRO 分派到基类实现。
import shapes


class Circle(shapes.Shape):
    def __init__(self, r: float) -> None:
        super().__init__("circle")
        self.area = 3.14159 * r * r

    def units(self) -> str:
        return super().describe() + " (circular)"


class Square(shapes.Shape):
    def __init__(self, s: float) -> None:
        super().__init__("square")
        self.area = s * s

    def units(self) -> str:
        return super().describe() + " (square)"


c = Circle(2.0)
print(c.name, round(c.area, 3))
print(c.units())
s = Square(3.0)
print(s.units())

# --- expect ---
# circle 12.566
# circle area=12.56636 (circular)
# square area=9.0 (square)
