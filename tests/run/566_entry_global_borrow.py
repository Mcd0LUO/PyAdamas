# 入口全局容器借用（0.474，PYADAMAS_BORROW_OPT）：语义对拍
#
# 形态：模块顶层 for 循环，容器与循环变量都是「入口私有」堆全局。
# 借用后循环变量不再持有元素引用（省掉每轮 inc/dec），元素由容器持有。
# 本用例钉死 5 条：命中形态的输出、循环后读循环变量的值、append-only 根、
# 循环体内删除容器时必须拒绝借用、容器被别的函数改过时必须拒绝借用。

class Shape:
    def __init__(self, w: int) -> None:
        self.w = w
    def area(self) -> int:
        return self.w

shapes = []
for i in range(6):
    shapes.append(Shape(i))

# 1) 命中形态：嵌套循环，内层每轮只读借用元素
total = 0
for r in range(3):
    for s in shapes:
        total += s.area()
print(total)

# 2) 循环结束后循环变量仍是最后一个元素（借用只在函数出口清零全局）
s2 = None
for s2 in shapes:
    pass
print(s2.area())

# 3) append-only 根：循环体内向容器 append 仍可借用
more = []
for i in range(3):
    more.append(str(i))
cnt = 0
for x in more:
    more.append(x)
    cnt += 1
    if cnt > 10:
        break
print(cnt)

# 4) 循环体内 pop：容器被删除 → 必须拒绝借用（借用会读到已释放元素）
z = []
for i in range(5):
    z.append(Shape(i))
picked = 0
for y in z:
    z.pop()
    picked += 1
    if len(z) == 0:
        break
print(picked)

# 5) 容器被别的函数写过 → 私有性不成立 → 必须拒绝借用
shapes2 = []
for i in range(4):
    shapes2.append(Shape(i))
def add_one() -> None:
    shapes2.append(Shape(9))
add_one()
total2 = 0
for s3 in shapes2:
    total2 += s3.area()
print(total2)

# --- expect ---
# 45
# 5
# 11
# 3
# 15
