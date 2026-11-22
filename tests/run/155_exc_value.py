# 异常值与 dunder 返回：富异常、多异常、类值异常
# 0.382 合并自 3 个文件：155_dunder_ret.py, 157_rich_exc.py, 159_exc_multi.py
# 每段包在独立函数里，避免同名变量/类跨段冲突。

# ── 原 155_dunder_ret.py ──
def _case_1() -> None:
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

# ── 原 157_rich_exc.py ──
def _case_2() -> None:
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

# ── 原 159_exc_multi.py ──
def _case_3() -> None:
    # 内置异常多参构造：args 元组还原、raise→except 与 CPython 一致。
    e = ValueError('x', 'y', 'z')
    print(len(e.args), e.args[0], e.args[1], e.args[2])

    s = ValueError('only')
    print(len(s.args), s.args[0])

    def f():
        try:
            raise ValueError(10, 'msg', 'path')
        except ValueError as err:
            print('caught', len(err.args), err.args[0], err.args[1], err.args[2])
    f()

    try:
        raise RuntimeError('a', 'b')
    except RuntimeError as r:
        print('rt', len(r.args), r.args[0])

_case_1()
_case_2()
_case_3()

# --- expect ---
# <1,2>
# 2
# True False
# 1
# -1
# True False
# -3
# negative
# -3:negative
# -6
# 1 ('args-ok',)
# 3 x y z
# 1 only
# caught 3 10 msg path
# rt 2 a
