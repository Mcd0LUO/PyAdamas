# 539_enum_abc.py —— enum 成员对象 + abc 抽象类（与 CPython 对拍）
from abc import ABC, abstractmethod
from enum import Enum


class Color(Enum):
    RED = 1
    GREEN = 2
    BLUE = 3


class Shape(ABC):
    @abstractmethod
    def area(self) -> int:
        return 0


class Square(Shape):
    side: int

    def __init__(self, side: int) -> None:
        self.side = side

    def area(self) -> int:
        return self.side * self.side


print("name_value", Color.RED.name, Color.RED.value)
print("lookup", Color(2).name, Color(3).value)
print("identity", Color.RED == Color(1), Color.RED is Color(1))
print("neq", Color.RED == Color.GREEN)
print("len", len(list(Color)))
for c in Color:
    print("iter", c.name, c.value)
print("names", [c.name for c in Color])

s = Square(3)
print("abc", s.area(), isinstance(s, Shape))

# --- expect ---
# name_value RED 1
# lookup GREEN 3
# identity True True
# neq False
# len 3
# iter RED 1
# iter GREEN 2
# iter BLUE 3
# names ['RED', 'GREEN', 'BLUE']
# abc 9 True
