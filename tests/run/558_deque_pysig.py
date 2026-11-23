# collections.deque 原生类方法表对拍（CPython 3.14 同形脚本）
# 已知差异：PA 的 maxlen 无界时为 -1（CPython 是 None），用兼容判定对齐。
# 构造器保留在 builtin_sigs.cpp（pysig 默认值/静态类型无法等价表达，见 cookbook）。
from collections import deque

d = deque([1, 2, 3])
print("init", list(d), d.maxlen is None or d.maxlen == -1)
d.append(4)
d.appendleft(0)
print("append", list(d))
print("pop", d.pop(), d.popleft())
d.extend([5, 6])
d.extendleft([-1, -2])
print("extend", list(d))
d.rotate(2)
print("rotate", list(d))
print("count", d.count(2), d.index(2))
d.remove(2)
print("remove", list(d))
d.insert(1, 99)
print("insert", list(d))
d.reverse()
print("reverse", list(d))
c = d.copy()
print("copy", list(c), c is d)
d.clear()
print("clear", list(d))

# --- expect ---
# init [1, 2, 3] True
# append [0, 1, 2, 3, 4]
# pop 4 0
# extend [-2, -1, 1, 2, 3, 5, 6]
# rotate [5, 6, -2, -1, 1, 2, 3]
# count 1 5
# remove [5, 6, -2, -1, 1, 3]
# insert [5, 99, 6, -2, -1, 1, 3]
# reverse [3, 1, -1, -2, 6, 99, 5]
# copy [3, 1, -1, -2, 6, 99, 5] False
# clear []
