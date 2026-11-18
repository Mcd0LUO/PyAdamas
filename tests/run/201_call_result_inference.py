# The leaf starts with unknown parameters. Its final result type must reach
# callers, their locals, and the module's enclosing arithmetic/formatting.
def top(value):
    return middle(value) * 3


def middle(value):
    result = leaf(value)
    return result + 0.5


def leaf(value):
    return value + value


a = top(1.25)
print(a, a + 1)
print(f"{leaf(1.25):.2f}")


def make(value) -> list:
    return [value]


xs = make(1.25)
print(xs[0], xs[0] * 2)


class Adder:
    def add(self, base: int, *, value):
        return base + value


adder = Adder()
print(adder.add(2, value=1.25))


def explicit_float(value: float) -> float:
    return value + 0.5


print(explicit_float(2), explicit_float(2.25))

# --- expect ---
# 9.0 10.0
# 2.50
# 1.25 2.5
# 3.25
# 2.5 2.75
