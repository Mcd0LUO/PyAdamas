class Point:
    x: int
    y: int
    z: int = 5

    def __init__(self, x: int, y: int = 0) -> None:
        self.x = x
        self.y = y

    def norm2(self) -> int:
        return self.x * self.x + self.y * self.y

    def move(self, dx: int, dy: int) -> None:
        self.x = self.x + dx
        self.y = self.y + dy

class Base:
    tag: str = "base"

    def label(self) -> str:
        return self.tag

class Child(Base):
    n: int = 0

    def __init__(self, n: int = 2, tag: str = "child") -> None:
        self.n = n
        self.tag = tag

    def total(self, extra: int) -> int:
        return self.n + extra


def make_point(x: int, y: int) -> Point:
    return Point(x, y)
