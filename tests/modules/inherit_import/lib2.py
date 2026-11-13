# 用前置小类把模块的方法槽推进到 5/6，让 Mixin 的 value 与 lib.Base.value
# 落在不同槽，专门验证跨模块多继承的槽别名。
class Pad0:
    def p0(self) -> int:
        return 0


class Pad1:
    def p1(self) -> int:
        return 1


class Pad2:
    def p2(self) -> int:
        return 2


class Pad3:
    def p3(self) -> int:
        return 3


class Pad4:
    def p4(self) -> int:
        return 4


class Mixin:
    def value(self) -> int:
        return 2

    def mix(self) -> str:
        return "mixin"
