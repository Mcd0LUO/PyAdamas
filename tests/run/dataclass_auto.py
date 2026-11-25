# @dataclass 按需自动开（0.555）：源码出现 dataclass 时自动展开，不需要 PYADAMAS_LANG_GAPS。
from dataclasses import dataclass


@dataclass
class P:
    x: int
    y: int


@dataclass
class WithDefault:
    name: str
    n: int = 7


p = P(1, 2)
print(p.x, p.y)
print(p)
print(p == P(1, 2))
w = WithDefault("a")
print(w.name, w.n)
print(w)

# --- expect ---
# 1 2
# P(x=1, y=2)
# True
# a 7
# WithDefault(name='a', n=7)
