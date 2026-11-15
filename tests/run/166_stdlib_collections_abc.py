"""stdlib/collections/abc.py：import 与注解可用（语义边界见模块 docstring）。"""
import collections
from collections.abc import Iterable, Mapping, Sequence


def total(xs: Sequence) -> int:
    s = 0
    for x in xs:
        s += x
    return s


def key_count(d: Mapping) -> int:
    return len(d)


d = collections.deque([1, 2, 3])
print(total([1, 2, 3]), key_count({"a": 1}))
print(len(d), d[0])
print(isinstance([1], Iterable))

# --- expect ---
# 6 1
# 3 1
# False
