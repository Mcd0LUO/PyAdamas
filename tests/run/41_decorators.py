class P:
    _x: int

    def __init__(self, x: int) -> None:
        self._x = x

    @property
    def x(self) -> int:
        return self._x

    @x.setter
    def x(self, v: int) -> None:
        self._x = v * 2

    @x.deleter
    def x(self) -> None:
        self._x = 0

    @staticmethod
    def add(a: int, b: int) -> int:
        return a + b

    def via_self(self, a: int) -> int:
        return self.add(a, 10)


class Q(P):
    pass


class R(P):
    @staticmethod
    def add(a: int, b: int) -> int:
        return a * b


q = Q(3)
print(q.x)
q.x = 5
print(q.x)
del q.x
print(q.x)
print(P.add(1, 2), q.add(3, 4), q.via_self(1))
print(R.add(3, 4), R(1).add(3, 4), R(1).via_self(2))

# --- expect ---
# 3
# 10
# 0
# 3 7 11
# 12 12 20
