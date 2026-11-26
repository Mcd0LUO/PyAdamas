# 0.580：模块级字面量常量当默认值（def f(a=_ONE)）编译期折叠
_ONE = 1
LIMIT = 10
NAME = "x"

def f(a: int = _ONE, b: int = LIMIT) -> int:
    return a + b

def g(s: str = NAME) -> str:
    return s

def h(*, k: int = LIMIT) -> int:
    return k

print(f(), f(2, 3), g(), h(), h(k=7))

# --- expect ---
# 11 5 x 10 7
