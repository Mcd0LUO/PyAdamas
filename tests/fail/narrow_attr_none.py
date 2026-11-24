# expect: 'b' may be None here; check it first
# 属性访问同样要求先判空（0.498）。
class Box:
    v: int

def f(b: Box | None) -> int:
    return b.v