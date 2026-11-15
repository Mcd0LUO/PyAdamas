# 方法体 / 嵌套函数里的单层 catch-all try：形状判定与函数种类无关。
class Box:
    def __init__(self, n: int) -> None:
        self.n = n

    def boom(self) -> None:
        try:
            raise ValueError("method")
        except Exception as e:
            print("method", e, self.n)
        print("method after")

    def quiet(self) -> None:
        try:
            print("quiet", self.n)
        except Exception:
            print("never")


def outer() -> None:
    def inner() -> None:
        try:
            raise KeyError("nested")
        except Exception as e:
            print("nested", e)
        print("nested after")

    inner()


b: Box = Box(7)
b.boom()
b.quiet()
outer()

# --- expect ---
# method method 7
# method after
# quiet 7
# nested nested
# nested after
