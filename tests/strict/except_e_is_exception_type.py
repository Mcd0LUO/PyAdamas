# expect: argument 'x' of 'need_int' expects 'int', got 'ValueError'
# except E as e 的 e 静态类型就是 E（不是 unknown）：当成别的类型传参会报错。
# 这条钉住「e 有类型」，与 tests/run/except_binds_exception.py 的正向用例一对。


def need_int(x: int) -> None:
    print(x)


try:
    raise ValueError("v")
except ValueError as e:
    need_int(e)
