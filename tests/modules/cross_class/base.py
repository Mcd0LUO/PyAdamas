class Point:
    x: int
    y: int
    def __init__(self, x: int, y: int) -> None:
        self.x = x
        self.y = y
    def norm(self) -> int:
        return self.x * self.x + self.y * self.y
    def move(self, dx: int, dy: int) -> None:
        self.x = self.x + dx
        self.y = self.y + dy


def make(x: int, y: int) -> Point:
    return Point(x, y)
