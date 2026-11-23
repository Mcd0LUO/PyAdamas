import base


class Point3(base.Point):
    z: int
    def __init__(self, x: int, y: int, z: int) -> None:
        super().__init__(x, y)
        self.z = z
    def norm3(self) -> int:
        return self.x * self.x + self.y * self.y + self.z * self.z
