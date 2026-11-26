# A8-3 的跨模块类属性用例：类属性（类体里不带动画的赋值）要能跨模块读。
class C:
    LIMIT = 42
    NAME = "hi"
    TAGS = "abc"

    def get_limit(self) -> int:
        return C.LIMIT

    def bump(self) -> None:
        C.LIMIT = C.LIMIT + 1


class Base:
    X = 7


class Sub(Base):
    pass


class Annotated:
    LIMIT: int = 42     # 带标注 = 实例字段，不是类属性（A8-1 的口径）
