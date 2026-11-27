from typing import Optional

class C:
    def __init__(self) -> None:
        self.v = 8
    def m(self) -> int:
        return self.v

# 0.679：循环体里的 assert 窄化不能泄漏到循环之后（块未执行时尤其危险）。
# 之前 while/for/try 体没有恢复点，`return x.m()` 会把 None 当类指针解引用。

def loop_zero(x: Optional[C], n: int) -> int:
    i = 0
    while i < n:
        assert x is not None
        i = i + 1
    if x is None:
        return 0
    return x.m()

def straight(x: Optional[C]) -> int:
    assert x is not None
    return x.m()

print(loop_zero(None, 0), loop_zero(C(), 2), straight(C()))
# --- expect ---
# 0 8 8
