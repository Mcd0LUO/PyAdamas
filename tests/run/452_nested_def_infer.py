# 452: 嵌套 def 的未标注形参也要从调用点回填类型。
# 以前只登记模块级函数和方法进重查表，函数体内定义的 def 拿不到实参类型，
# 形参停在 unknown —— 方法调用会在降级期直接报「method 'x' is not supported」。


class Box:
    def __init__(self, v: int) -> None:
        self.v: int = v

    def get(self) -> int:
        return self.v


def outer() -> None:
    # ① 形参拿到 str，才能调 .upper()
    def shout(s) -> str:
        return s.upper()

    # ② 形参拿到类实例，才能读字段/调方法
    def unwrap(b) -> int:
        return b.get() + b.v

    # ③ 形参是容器，才能下标/长度
    def head(xs) -> int:
        return xs[0] + len(xs)

    # ④ 三层嵌套，类型要能一路回填
    def level1(n) -> int:
        def level2(m) -> int:
            return m * 2

        return level2(n)

    word: str = "abc"
    print(shout(word))
    print(unwrap(Box(7)))
    nums: list[int] = [10, 20]
    print(head(nums))
    print(level1(21))


outer()
# --- expect ---
# ABC
# 14
# 12
# 42
