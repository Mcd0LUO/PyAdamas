# 564_for_iter_hold.py —— for 迭代源持有：体内重绑迭代名不得悬垂
#
# 历史 bug：`for x in a: a = [...]` 里序列值 a 只是借用（load.slot /
# load.global 不增计数），体内重绑 a 会 dec 掉最后一份引用并释放旧列表，
# 后续迭代读已释放对象。实测 a=[1,2,3] 只迭代出 2 个元素（释放块被下次
# 分配复用，len 变成别的值），ASan 因分配器池化未报错。
# 修复：循环开始前对借用形态的序列补一份引用并登记进临时栈，随语句边界
# 的 rc_cleanup 释放；新分配值已由临时栈持有，不重复持有。

a = [1, 2, 3]
out = []
for x in a:
    a = []
    out.append(x)
print(out, a)

b = [4, 5, 6]
s = 0
for y in b:
    b = [9]
    s = s + y
print(s, b)

def rebind(xs: list[int]) -> list[int]:
    ys = []
    for x in xs:
        xs = [7]
        ys.append(x)
    return ys

print(rebind([1, 2, 3]))

def shrink(xs: list[int]) -> list[int]:
    ys = []
    for x in xs:
        ys.append(x)
        xs.pop()
    return ys

print(shrink([1, 2, 3, 4]))

# 属性来源同样是借用：体内重绑宿主对象的字段也不能让序列悬垂
class Box:
    def __init__(self) -> None:
        self.items: list[int] = [10, 20, 30]

box = Box()
got = []
for v in box.items:
    got.append(v)
    box.items = []
print(got, box.items)

# 正常（不重绑）循环结果不变
total = 0
for q in [1, 2, 3, 4]:
    total += q
print(total)

def keep(xs: list[int]) -> int:
    acc = 0
    for x in xs:
        acc += x
    return acc
print(keep([5, 6, 7]))

# --- expect ---
# [1, 2, 3] []
# 15 [9]
# [1, 2, 3]
# [1, 2]
# [10, 20, 30] []
# 10
# 18
