# 跨模块继承的基类：Derived 在同一程序里 import 它并 super() 调它的方法。
class Shape:
    def __init__(self, name: str) -> None:
        self.name = name
        self.area = 0.0

    def describe(self) -> str:
        return self.name + " area=" + str(self.area)
