# 守卫式 devirt 语义钉子（PYADAMAS_INLINE_OPT）：单态/双态调用点、继承共享
# 方法、带实参的非叶子小方法（守卫直调）、异常穿过 devirt 调用点（EH 快路径
# 覆盖）、后定义子类流入基类静态类型调用点（守卫必须回退原 vcall）。
class Base:
    def __init__(self, x: int) -> None:
        self.x = x
    def get(self, i: int) -> int:
        return self.x + i
    def val(self) -> int:
        return self.x
    def boom(self) -> int:
        raise ValueError("boom")


class Sub(Base):
    def val(self) -> int:
        return self.x * 2


class Sub2(Base):
    pass


def use(b: Base, i: int) -> int:
    return b.get(i) + b.val()


total: int = 0
objs: list[Base] = [Base(3), Sub(5), Sub2(7)]
for r in range(2000):
    for o in objs:
        total += use(o, r % 5)
print(total)

caught: int = 0
for r in range(100):
    try:
        objs[r % 3].boom()
    except ValueError as e:
        caught += 1
print(caught)


class Late(Base):
    def val(self) -> int:
        return self.x + 100


def use2(b: Base) -> int:
    return b.val()


late: Late = Late(1)
base: Base = Base(1)
print(use2(late))
print(use2(base))
print(use2(Sub(2)))

class Acc:
    def __init__(self, x: int) -> None:
        self.x = x
    def m(self) -> int:
        return self.x + helper(self.x)


def helper(x: int) -> int:
    return x * 2


acc: Acc = Acc(7)
gtotal: int = 0
for i in range(1000):
    gtotal += acc.m()
print(gtotal)


# --- expect ---
# 82000
# 100
# 101
# 1
# 4
# 21000
