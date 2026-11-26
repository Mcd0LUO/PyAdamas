# A8-4 的单文件口径：enum 类对象的 len() 与迭代（导入类走同一套判断）。
# len(Color) 在 0.653 之前是 typecheck 拒绝（object of type 'type[Color]'）,
# 现在按成员个数返回，CPython 口径。
from enum import Enum

class Color(Enum):
    RED = 1
    GREEN = 2
    BLUE = 3

print(len(Color))
for m in Color:
    print(m.name, m.value)
print([m.value for m in Color])
print(list(Color)[2].name)
print(Color(len(Color)).name)

# --- expect ---
# 3
# RED 1
# GREEN 2
# BLUE 3
# [1, 2, 3]
# BLUE
# BLUE
