"""stdlib/enum.py：Enum 成员对象（.name/.value/值反查）+ IntEnum/Flag 的 int 语义。"""
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


def describe(c: Color) -> str:
    if c is Color.RED:
        return "red"
    if c is Color.GREEN:
        return "green"
    return "?"

print(describe(Color.RED), describe(Color.GREEN))
print(Color.RED.name, Color.RED.value, Color.GREEN.name, Color.GREEN.value)
print(Color(2).name, Color(1).value)
print(Code.OK, Code.OK == 200, Code.ERR > 400)
print(Perm.READ, Perm.READ | Perm.WRITE)

# --- expect ---
# red green
# RED 1 GREEN 2
# GREEN 1
# 200 True True
# 4 6
