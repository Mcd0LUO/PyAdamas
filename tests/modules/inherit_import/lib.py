class Base:
    flag: str = "base"
    tag: str = "base"

    def __init__(self, tag: str = "base") -> None:
        self.tag = tag

    def label(self) -> str:
        return "base:" + self.tag

    def value(self) -> int:
        return 1

    @classmethod
    def make(cls, tag: str) -> "Base":
        return cls(tag)

    @classmethod
    def who(cls) -> str:
        return cls.__name__


class Middle(Base):
    def __init__(self, tag: str) -> None:
        super().__init__(tag)

    def value(self) -> int:
        return super().value() + 10


def make(tag: str) -> Middle:
    return Middle(tag)
