"""stdlib/typing.py：注解可编译 + cast/TYPE_CHECKING 可用。"""
import typing
from typing import Any, Dict, Iterable, List, Optional, Tuple, Union, cast


def total(xs: List[int]) -> int:
    s = 0
    for x in xs:
        s += x
    return s


def first(xs: List[int]) -> Optional[int]:
    if len(xs) == 0:
        return None
    return xs[0]


def keys(d: Dict[str, int]) -> List[str]:
    return list(d.keys())


def pair(t: Tuple[int, str]) -> int:
    return t[0]


def anyv(x: Any) -> int:
    return 1


n = cast(int, 3)
print(total([1, 2, 3]), first([]), first([9]))
print(keys({"a": 1, "b": 2}))
print(pair((7, "x")), anyv("s"), n + 1)
print(typing.TYPE_CHECKING)

# --- expect ---
# 6 None 9
# ['a', 'b']
# 7 1 4
# False
