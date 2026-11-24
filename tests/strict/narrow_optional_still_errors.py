# 对真实 Optional 的检查不能被削弱：标注成 int | None 的值直接参与运算仍要报错。
def f(x: int | None) -> int:
    return x + 1
