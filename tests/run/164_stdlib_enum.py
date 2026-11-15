"""stdlib/enum.py：类属性常量 + IntEnum/Flag 的 int 语义。"""
from enum import Enum, Flag, IntEnum, unique


@unique
class Color(Enum):
    RED = 1
    GREEN = 2


class Code(IntEnum):
    OK = 200
    ERR = 500


class Perm(Flag):
    READ = 4
    WRITE = 2


def describe(c: int) -> str:
    if c == Color.RED:
        return "red"
    if c == Color.GREEN:
        return "green"
    return "?"


print(describe(Color.RED), describe(Color.GREEN))
print(Color.RED, Color.GREEN)
print(Code.OK, Code.OK == 200, Code.ERR > 400)
print(Perm.READ, Perm.READ | Perm.WRITE)

# --- expect ---
# red green
# 1 2
# 200 True True
# 4 6
