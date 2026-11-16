# 用户类的一元 dunder：__neg__ / __pos__ / __invert__ / __abs__ / __bool__ / __len__。
# 这些以前在降级层没有分派，直接把对象指针当机器整数取负，再按结果的静态
# 类型（用户类 = 堆类型）做引用计数——野指针，运行期 SIGSEGV。
import operator


# 字段标注写在 __init__ 里（self.x: int = x）：AnnAssign 的接收者
# 类型没登记时，降级层会把它当成「不支持的赋值目标」直接报错。
class Ann:
    def __init__(self, x: int) -> None:
        self.x: int = x

    def __neg__(self) -> int:
        return -self.x


class Vec:
    x: int

    def __init__(self, x: int) -> None:
        self.x = x

    def __neg__(self) -> int:
        return -self.x

    def __pos__(self) -> int:
        return self.x

    def __invert__(self) -> int:
        return ~self.x

    def __abs__(self) -> int:
        return abs(self.x)


# 返回类型是用户类本身：结果必须是一个正常的堆对象，能被赋值、打印。
class Frac:
    n: int
    d: int

    def __init__(self, n: int, d: int) -> None:
        self.n = n
        self.d = d

    def __neg__(self) -> "Frac":
        return Frac(-self.n, self.d)

    def __pos__(self) -> "Frac":
        return Frac(self.n, self.d)

    def __abs__(self) -> "Frac":
        return Frac(abs(self.n), self.d)

    def __str__(self) -> str:
        return str(self.n) + "/" + str(self.d)


class Truthy:
    v: int

    def __init__(self, v: int) -> None:
        self.v = v

    def __bool__(self) -> bool:
        return self.v > 0


class Sized:
    n: int

    def __init__(self, n: int) -> None:
        self.n = n

    def __len__(self) -> int:
        return self.n


# 任务书里的字面复现形态：标注写在 __init__ 里
a: Ann = Ann(3)
print(-a)

v: Vec = Vec(3)
print(-v, +v, ~v, abs(v))
print(-Vec(-5), abs(Vec(-5)))

f: Frac = Frac(-3, 4)
print(-f, +f, abs(f))
g: Frac = -f
print(g, g.n, g.d)

# 结果是 int 时可以直接参与后续整数运算和类型标注。
n: int = -v
print(n, n * 2, n + 1)

# operator 模块的四个包装函数走同一条 dunder 分派。
print(operator.neg(v), operator.pos(v), operator.invert(v), operator.abs(v))
print(operator.neg(f), operator.pos(f), operator.abs(f))

# 真值：__bool__ 优先，其次 __len__ 非零，都没有才按指针判空。
print(bool(Truthy(1)), bool(Truthy(0)), not Truthy(1), not Truthy(0))
print(bool(Sized(2)), bool(Sized(0)), not Sized(2), not Sized(0))
if Truthy(5):
    print("truthy")
if not Truthy(-5):
    print("falsy")
print(len(Sized(7)))

# 同名 dunder 在不同类上互不影响。
print(-Vec(10) + -Vec(20), -Frac(1, 2))

# --- expect ---
# -3
# -3 3 -4 3
# 5 5
# 3/4 -3/4 3/4
# 3/4 3 4
# -3 -6 -2
# -3 3 -4 3
# 3/4 -3/4 3/4
# True False False True
# True False False True
# truthy
# falsy
# 7
# -30 -1/2
