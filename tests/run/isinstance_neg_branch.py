# isinstance 否定分支的窄化（0.529 P0-D）：`if isinstance(o, int): return` 之后，
# 后续语句只可能在 o 不是 int 时到达，联合要收窄到剩下的成员。
# 修前：typecheck 收窄了但降级只看到声明的 `int | str`，报
#   method 'upper' is not supported by the code generator yet。


class Box:
    def __init__(self, v: str) -> None:
        self.v = v

    def name(self) -> str:
        return self.v


def two(o: int | str) -> str:
    if isinstance(o, int):
        return "int"
    return o.upper()


def three(o: int | str | float) -> str:
    if isinstance(o, int):
        return "int"
    if isinstance(o, float):
        return "float"
    return o.strip()


def negated(o: int | str) -> str:
    if not isinstance(o, str):
        return "no"
    return o.strip()


def raised(o: int | str) -> str:
    if isinstance(o, int):
        raise ValueError("int")
    return o.upper()


def via_attr(box: Box, o: int | str) -> str:
    if isinstance(o, int):
        return box.name()
    return o.upper()


print(two(1), two("ab"))
print(three(1), three(2.5), three(" ab "))
print(negated(1), negated(" ab "))
print(raised(1) if False else "skip", raised("ab"))
print(via_attr(Box("b"), 3), via_attr(Box("b"), "cd"))

# --- expect ---
# int AB
# int float ab
# no ab
# skip AB
# b CD
