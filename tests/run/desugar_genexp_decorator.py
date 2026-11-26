# 0.638：推导式里的嵌套生成器 / 混合装饰器顺序
xs = [1, 2]
ys = [10, 20]
print([sum(x + y for y in ys) for x in xs])


class Mod:
    def outer(self, f):
        def w() -> int:
            return f() * 10
        return w


m = Mod()


def deco(f):
    def w() -> int:
        return f() + 1
    return w


@m.outer
@deco
def f() -> int:
    return 1


print(f())


@deco
@deco
def g() -> int:
    return 1


print(g())

# --- expect ---
# [32, 34]
# 20
# 3
