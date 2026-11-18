# Omitted defaults count as arguments. Unused defaults are not constraints.
def forward():
    return twice() * 3


def twice(value=1.25):
    return value + value


def override(value=1.25):
    return value + value


def suffix(name, /, *, tag="ok"):
    return name.upper() + tag.upper()


def label(value=1.25, /, **extra: int):
    return value * 2 + extra["value"]


print(twice(), twice(2.25), override(3), forward())
print(suffix("hi"), label(value=4))


class Example:
    def score(self, base: int, *, scale=1.25):
        return base * scale


obj = Example()
print(obj.score(3), Example.score(obj, 2))


def render(name, scale, *, tag="!"):
    return name.upper() + str(scale * 2) + tag


def pair() -> tuple[str, float]:
    print("pair evaluated")
    return ("xy", 1.25)


print(render(*pair()), render(*(), *("ab", 2.25), tag="?"))


def calc(first, second=1.25, /):
    return first * second


print(calc(*(3,)), calc(*(4, 2.25)))

# --- expect ---
# 2.5 4.5 6 7.5
# HIOK 6.5
# 3.75 2.5
# pair evaluated
# XY2.5! AB4.5?
# 3.75 9.0
