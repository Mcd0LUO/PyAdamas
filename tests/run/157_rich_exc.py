# 富异常类（有字段/方法的自定义异常类）作为普通类：字段、__init__、方法、
# raise + except as e 取回真实例。e.code / e.describe() 走普通类访问。
class MyError(ValueError):
    def __init__(self, code: int, name: str) -> None:
        super().__init__(name)
        self.code = code
        self.name = name

    def describe(self) -> str:
        return str(self.code) + ":" + self.name

    def dup(self) -> int:
        return self.code * 2


def f(x: int) -> int:
    if x < 0:
        raise MyError(x, "negative")
    return x


try:
    f(-3)
except MyError as e:
    print(e.code)
    print(e.name)
    print(e.describe())
    print(e.dup())


class Empty(ValueError):
    pass


try:
    raise Empty("args-ok")
except Empty as e:
    print(len(e.args), e.args)

# --- expect ---
# -3
# negative
# -3:negative
# -6
# 1 ('args-ok',)
