# A8-4 的跨模块 enum 用例：定义模块只放枚举，成员/构造/迭代都在导入方用。
from enum import Enum


class Color(Enum):
    RED = 1
    GREEN = 2
    BLUE = 3
