import kenum

# 按值反查成员
print(kenum.Color(1).name)
print(kenum.Color(2).value, kenum.Color(3).name)
print(kenum.Color(1) is kenum.Color.RED)

# 长度与迭代
print(len(kenum.Color))
for m in kenum.Color:
    print(m.name, m.value)
print([m.name for m in kenum.Color])
print(list(kenum.Color)[1].name)

# from-import 的类同口径
from kenum import Color
print(Color(2).name)
print(len(Color))
for m in Color:
    print(m.name)

# 成员身上的字段仍是类属性（A8-3）
print(kenum.Color.RED.value, kenum.Color.RED.name)
print(isinstance(kenum.Color.RED, kenum.Color))

# --- expect ---
# RED
# 2 BLUE
# True
# 3
# RED 1
# GREEN 2
# BLUE 3
# ['RED', 'GREEN', 'BLUE']
# GREEN
# GREEN
# 3
# RED
# GREEN
# BLUE
# 1 RED
# True
