# 边角：__format__ / __bool__ / __reversed__ / iter / hasattr / setattr、
# 类体 lambda、函数内定义类、C.m(obj, ...)。


class Fmt:
    def __format__(self, spec: str) -> str:
        return "[" + spec + "]"


class Truthy:
    def __bool__(self) -> bool:
        return False


class Countdown:
    def __iter__(self):
        return iter([1, 2, 3])


class Rev:
    def __reversed__(self):
        return iter([3, 2, 1])


class Box:
    def __init__(self) -> None:
        self._v = 0

    @property
    def v(self) -> int:
        return self._v

    @v.setter
    def v(self, x: int) -> None:
        self._v = x

    def get(self) -> int:
        return self._v


class Lam:
    f = lambda self, x: x * 2


def nested():
    class Inner:
        def m(self) -> int:
            return 7

    return Inner().m()


def main() -> None:
    print(format(Fmt(), "x"))
    print(f"{Fmt():>4}")

    t = Truthy()
    print(bool(t))
    if t:
        print("truthy")
    else:
        print("falsy")
    print(t or "fallback")

    total = 0
    for v in iter(Countdown()):
        total = total + v
    print("iter", total)

    print("rev", list(reversed(Rev())))

    b = Box()
    setattr(b, "v", 9)
    print("setattr", b.v, getattr(b, "v"))

    print("hasattr", hasattr("ab", "upper"), hasattr(3, "upper"), hasattr(b, "v"))

    print("lambda", Lam().f(3))
    print("nested", nested())
    print("crecv", Box.get(Box()))


main()

# --- expect ---
# [x]
# [>4]
# False
# falsy
# fallback
# iter 6
# rev [3, 2, 1]
# setattr 9 9
# hasattr True False True
# lambda 6
# nested 7
# crecv 0
