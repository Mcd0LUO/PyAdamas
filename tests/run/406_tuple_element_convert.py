# 406: 元组元素类型转换（0.359）。
# 元组字面量按推断类型构造，边界上目标元素类型不同时必须逐元素重建，
# 否则 int 位模式会被当成 double 或 big 指针。
def floats() -> tuple[float, float]:
    return 1, 2


def bigs() -> tuple[int, big, int]:
    return 0, 7, 0


def takes(t: tuple[float, float]) -> float:
    return t[0] + t[1]


def takes_big(t: tuple[big, big]) -> big:
    return t[0] * t[1]


def nested() -> tuple[tuple[float, float], int]:
    return (1, 2), 3


print(int(floats()[0]), int(floats()[1]))
a, b, c = bigs()
print(a, b, c, b + 1)
print(int(takes((3, 4))))
print(takes_big((10, 20)))
n = nested()
print(int(n[0][0]), int(n[0][1]), n[1])
lit: tuple[float, float] = (5, 6)
print(int(lit[0]), int(lit[1]))
src: tuple[int, int] = (7, 8)
conv: tuple[float, float] = src
print(int(conv[0]), int(conv[1]))
print(int(sum(floats())))
# --- expect ---
# 1 2
# 0 7 0 8
# 7
# 200
# 1 2 3
# 5 6
# 7 8
# 3
