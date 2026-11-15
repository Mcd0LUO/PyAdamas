# 跨模块异常类：定义模块只负责定义/抛出，导入方负责 except/raise 名字位置。
class LibError(ValueError):
    pass


class SubError(LibError):
    pass


class RichError(ValueError):
    code: int

    def __init__(self, code: int, name: str) -> None:
        super().__init__(name)
        self.code = code
        self.name = name

    def describe(self) -> str:
        return str(self.code) + ":" + self.name


def boom() -> None:
    raise LibError("from lib")


def boom_sub() -> None:
    raise SubError("from lib sub")


def boom_rich() -> None:
    raise RichError(7, "rich")
