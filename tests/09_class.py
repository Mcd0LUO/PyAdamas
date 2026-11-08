# 类的类型检查正例：布局、继承、字段、方法签名

class Shape:
    name: str
    sides: int = 0

    def __init__(self, name: str, sides: int = 0) -> None:
        self.name = name
        self.sides = sides

    def area(self) -> float:
        return 0.0

    def describe(self) -> str:
        return self.name


class Square(Shape):
    side: float

    def __init__(self, side: float) -> None:
        self.name = "square"
        self.sides = 4
        self.side = side

    def area(self) -> float:
        return self.side * self.side


class Cube(Square):
    def area(self) -> float:
        # 调父类实现要走 self，而不是 super()：super 还没做
        return self.side * self.side * 6.0


s: Shape = Square(2.0)
print(s.describe(), s.sides, s.area())

shapes: list[Shape] = [Shape("dot"), Square(3.0), Cube(1.0)]
total: float = 0.0
for one in shapes:
    total = total + one.area()
print(total)

# 可选字段
class Maybe:
    tag: str
    inner: "Maybe | None"

    def __init__(self, tag: str) -> None:
        self.tag = tag
        self.inner = None

m: Maybe = Maybe("a")
if m.inner is not None:
    print(m.inner.tag)

# 字段是容器
class Bag:
    items: list[int] = []

    def add(self, v: int) -> None:
        self.items.append(v)

b: Bag = Bag()
b.add(1)
b.add(2)
print(len(b.items))

# 方法返回自身的类型
class Chain:
    n: int = 0

    def inc(self) -> "Chain":
        self.n = self.n + 1
        return self

c: Chain = Chain()
c.inc().inc().inc()
print(c.n)
