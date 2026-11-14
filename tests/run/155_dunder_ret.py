# dunder 的返回类型在函数体推理完之后才判：未标注的方法只要推出来是
# str/int/bool 就通过，推不出来（unknown）也放过。getattr 的第三个参数
# 是「静态布局里没有这个字段」时的结果。
class Point:
    def __init__(self, x: int, y: int) -> None:
        self.x = x
        self.y = y

    def __repr__(self):
        return "<" + str(self.x) + "," + str(self.y) + ">"

    def __len__(self):
        return 2

    def __bool__(self):
        return self.x != 0


p = Point(1, 2)
print(repr(p))
print(len(p))
print(bool(p), bool(Point(0, 0)))
print(getattr(p, "x", 0))
print(getattr(p, "z", -1))
print(hasattr(p, "y"), hasattr(p, "z"))

# --- expect ---
# <1,2>
# 2
# True False
# 1
# -1
# True False
