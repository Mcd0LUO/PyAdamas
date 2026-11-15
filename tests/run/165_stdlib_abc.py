"""stdlib/abc.py：ABC 基类 + abstractmethod 声明式用法。"""
from abc import ABC, ABCMeta, abstractclassmethod, abstractmethod


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


s = Square(3)
print(s.area())
print(isinstance(s, Shape))

# --- expect ---
# 9
# True
