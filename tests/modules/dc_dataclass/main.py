# pyadamas: lang-gaps
# P4 块4：跨模块 dataclass——合成方法进导出接口，导入方构造/eq/repr 全链路。
from lib import Item, make

i = Item("a")
print(i)
j = make("b")
print(j)
print(i == Item("a"))
print(i == j)
print(i.name, i.qty)

# --- expect ---
# Item(name='a', qty=1)
# Item(name='b', qty=5)
# True
# False
# a 1
