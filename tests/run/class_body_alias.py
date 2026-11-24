# 类体里的「方法别名」（CPython 合法，stdlib 里非常常见）：
#   _weakrefset.py 的 __sub__ = difference、doctest.py 的 __str__ = shortDescription、
#   multiprocessing/process.py 的 pid = ident（property 别名）。
# CPython 的类体是顺序求值，def 当场绑定名字；pyadamas 的方法不是一等值，
# 别名在编译期建成「共享实现的两个方法槽」，dunder 分派与普通调用都走同一份。


class A:
    def short(self) -> str:
        return "x"

    __str__ = short          # dunder 别名：str(a) / print(a) / a.__str__()
    alias = short            # 普通方法别名


class S:
    def __init__(self, n: int) -> None:
        self.n: int = n

    def difference(self, other: S) -> int:
        return self.n - other.n

    def _len(self) -> int:
        return self.n

    __sub__ = difference     # 运算符 dunder 别名
    __len__ = _len


class P:
    def __init__(self) -> None:
        self._pid: int = 42

    @property
    def ident(self) -> int:
        return self._pid

    pid = ident              # property 别名：读 p.pid 走同一个 getter


class C:
    def __init__(self) -> None:
        self.v: int = 7

    @classmethod
    def make(cls) -> "C":
        return C()

    @staticmethod
    def helper(a: int) -> int:
        return a + 1

    create = make            # classmethod 别名
    util = helper            # staticmethod 别名


a: A = A()
print(a.__str__())
print(str(a))
print(a)
print(a.alias())

s1: S = S(9)
s2: S = S(4)
print(s1 - s2)
print(len(s1))

p: P = P()
print(p.pid)
print(p.ident)

c: C = C.create()
print(c.v)
print(C.util(41))

# --- expect ---
# x
# x
# x
# x
# 5
# 9
# 42
# 42
# 7
# 42
