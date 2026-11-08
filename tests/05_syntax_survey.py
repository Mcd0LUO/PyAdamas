"""语法全览：把支持的语法都过一遍，确保 parser 不留死角。"""

import os
import sys as system
from typing import Optional
from . import sibling

# ── 字面量 ──
i = 0
h = 0xFF
o = 0o17
b = 0b1010
big = 1_000_000
f1 = 1.5
f2 = .5
f3 = 1.
f4 = 1e10
f5 = 1.5e-3
s1 = "double"
s2 = 'single'
s3 = """triple
line"""
s4 = r"raw\n"
s5 = f"f-string {i + 1}"
t = True
f = False
n = None

# ── 运算符 ──
a = 1 + 2 - 3 * 4 / 5 // 6 % 7 ** 2
a = 1 << 2 | 3 & 4 ^ 5
a = ~1
a = -1
a = +1
c1 = 1 < 2 <= 3 > 0 >= -1 == 1 != 2
c2 = 1 is None
c3 = 1 is not None
c4 = 1 in [1]
c5 = 1 not in [1]
c6 = not True
c7 = True and False or True

# ── 容器 ──
lst = [1, 2, 3]
tup = (1, 2, 3)
one = (1,)
empty_t = ()
st = {1, 2, 3}
dct = {"a": 1, "b": 2}
unpack_l = [*lst, 4]
unpack_d = {**dct, "c": 3}
idx = lst[0]
neg = lst[-1]
sl = lst[1:3]
sl2 = lst[::2]
sl3 = lst[::-1]

# ── 推导式 ──
lc = [v for v in lst if v > 1]
sc = {v for v in lst}
dc = {v: v * 2 for v in lst}
ge = (v for v in lst)
nested_c = [(x, y) for x in range(3) for y in range(3) if x != y]

# ── 函数 ──
def plain():
    pass


def typed(a: int, b: float = 1.0, *args, key: str = "k", **kwargs) -> Optional[int]:
    local = a + 1
    global i
    i = local
    yield local
    return local


def posonly(a, b, /, c):
    return a


lam = lambda x, y=1: x + y


@staticmethod
def decorated(x: int) -> int:
    return x


# ── 控制流 ──
if i:
    pass
elif not i:
    pass
else:
    pass

while i < 10:
    i += 1
    if i == 3:
        continue
    if i == 8:
        break
else:
    pass

for item in lst:
    pass
else:
    pass

for k, v in dct.items():
    pass

for idx2, val in enumerate(lst):
    pass

# 三元
ternary = "yes" if i else "no"

# 海象
if (found := len(lst)) > 0:
    pass

# ── 类 ──
class Base:
    pass


class Child(Base, metaclass=type):
    attr: int = 1

    def method(self, x: int) -> int:
        return x


# ── 异常 ──
try:
    risky = 1 / 0
except ZeroDivisionError as e:
    pass
except (ValueError, TypeError):
    pass
else:
    pass
finally:
    pass


# ── with ──
with open("f") as fh, open("g") as gh:
    pass

# ── 其他 ──
assert i >= 0, "must be non-negative"
del a
if True: pass

# ── 续行 ──
long_sum = 1 + \
           2 + \
           3

paren_sum = (1 +
             2 +
             3)

# ── 中文标识符 ──
变量 = 1
中文名 = 变量 + 1
