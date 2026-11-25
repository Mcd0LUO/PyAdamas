# expect: takes no arguments (no __init__)
# pyadamas: no-lang-gaps
# lang-gaps 门控关时 @dataclass 只是恒等装饰器，不会合成 __init__。
# 放行带参构造等于静默丢参（P(1) 的 x 是 0），必须在编译期拒绝。
from dataclasses import dataclass

@dataclass
class P:
    x: int

p = P(1)
print(p.x)
