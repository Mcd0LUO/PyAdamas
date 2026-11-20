# pyadamas: lang-gaps
"""351: P4 块4 @dataclass 编译期展开，CPython 3.14 逐字节对拍。

v1 范围：单类 + 字面量默认值 + 标量/str/bytes/容器字段。负面（frozen/
order/kw_only/field()/继承/可变默认/默认序违规）在 tests/fail/352_*。
避开 pa 边界：None 默认进容器槽（pa 类型系统一般规则）、嵌套 set 元素
repr（探针口径）、dataclass 实例当 dict 键（__eq__ 无 __hash__ →
unhashable，编译期报错，CPython 是运行期 TypeError）。
"""
from dataclasses import dataclass


@dataclass
class Point:
    x: int
    y: int = 3


p = Point(1)
print(p.x, p.y)
print(p)
print(repr(p))
q = Point(1, 3)
r = Point(2, 5)
print(p == q)
print(p == r)
print(p != r)
print(p == 5)
print(p == "x")
s = Point(x=10, y=20)
print(s)
p.x = 99
print(p)


@dataclass
class Cfg:
    name: str = "def"
    ratio: float = 0.5
    on: bool = True
    n: int = 0


c = Cfg()
print(c)
c2 = Cfg("x", 1.5, False, 7)
print(c2)
print(c == Cfg())
print(c == c2)


@dataclass
class S:
    name: str
    tags: list[str]
    meta: dict[str, int]


a = S("a", ["t1", "t2"], {"k": 1})
b = S("a", ["t1", "t2"], {"k": 1})
d = S("b", [], {})
print(a)
print(a == b)
print(a == d)
print(a.tags)
a.tags.append("t3")
print(a)
print(a == b)


@dataclass
class T:
    pair: tuple[int, str]
    st: set[int]


t = T((1, "a"), {1, 2})
print(t)
print(t == T((1, "a"), {1, 2}))
print(t == T((2, "b"), {3}))


@dataclass
class Nest:
    grid: list[list[int]]
    idx: dict[str, list[int]]


n1 = Nest([[1, 2], [3]], {"a": [1]})
n2 = Nest([[1, 2], [3]], {"a": [1]})
n3 = Nest([[1, 9], [3]], {"a": [1]})
print(n1)
print(n1 == n2)
print(n1 == n3)


@dataclass
class B:
    data: bytes
    flag: bool


bb = B(b"hi", False)
print(bb)
print(bb == B(b"hi", False))
print(bb == B(b"ho", True))


@dataclass
class Q:
    text: str


print(Q("it's"))
print(Q('say "hi"'))
print(Q("a\nb"))
print(Q("") == Q(""))


@dataclass
class Empty:
    pass


e = Empty()
print(e)
print(e == Empty())
print(e != Empty())


@dataclass
class User:
    x: int

    def __repr__(self) -> str:
        return "custom"

    def __eq__(self, other) -> bool:
        return True


u = User(1)
print(u)
print(u == 99)


@dataclass
class InitOnly:
    x: int

    def __init__(self, x: int):
        self.x = x * 2


i = InitOnly(3)
print(i.x)
print(i)


def local_dc():
    @dataclass
    class L:
        v: int = 7

    return L()


# 函数内类：pa 静态编译一次（CPython 每次调用重新执行 class 语句，跨调用
# 实例的类对象不同 → eq False）；repr 的 qualname 口径一致，跨调用 eq 差异
# 文档化（docs/lang-gaps-landing.md §8 块4 边界）。
print(local_dc())
# --- expect ---
# 1 3
# Point(x=1, y=3)
# Point(x=1, y=3)
# True
# False
# True
# False
# False
# Point(x=10, y=20)
# Point(x=99, y=3)
# Cfg(name='def', ratio=0.5, on=True, n=0)
# Cfg(name='x', ratio=1.5, on=False, n=7)
# True
# False
# S(name='a', tags=['t1', 't2'], meta={'k': 1})
# True
# False
# ['t1', 't2']
# S(name='a', tags=['t1', 't2', 't3'], meta={'k': 1})
# False
# T(pair=(1, 'a'), st={1, 2})
# True
# False
# Nest(grid=[[1, 2], [3]], idx={'a': [1]})
# True
# False
# B(data=b'hi', flag=False)
# True
# False
# Q(text="it's")
# Q(text='say "hi"')
# Q(text='a\nb')
# True
# Empty()
# True
# False
# custom
# True
# 6
# InitOnly(x=6)
# local_dc.<locals>.L(v=7)
