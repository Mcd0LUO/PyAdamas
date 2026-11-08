# 对象模型：类、字段、方法、继承、多态
#
# 这一组钉住的是对象层最容易出错的地方：
#   1. 字段偏移（含继承时的前缀布局）
#   2. vtable 槽位（覆盖沿用父类槽，新增追加到末尾）
#   3. 用父类类型持有子类实例时的虚调用
#   4. 8 个参数：超出寄存器的部分走栈，self 仍占第一个
#   5. Optional 字段的 None 检查
#   6. 字段初值在构造时求值，_子类覆盖 __init__ 也要初始化到


class Point:
    x: int
    y: int

    def __init__(self, x: int, y: int) -> None:
        self.x = x
        self.y = y

    def norm2(self) -> int:
        return self.x * self.x + self.y * self.y

    def shift(self, d: int) -> None:
        self.x = self.x + d
        self.y = self.y + d

    def plus(self, o: Point) -> Point:
        return Point(self.x + o.x, self.y + o.y)


p: Point = Point(3, 4)
print("point", p.x, p.y, p.norm2())
p.shift(1)
print("shift", p.x, p.y, p.norm2())

q: Point = p.plus(Point(10, 20))
print("plus", q.x, q.y)


# ── 继承与方法覆盖 ────────────────────────────────────────────────

class Animal:
    legs: int = 4

    def speak(self) -> str:
        return "..."

    def describe(self) -> str:
        return self.speak()

    def leg_count(self) -> int:
        return self.legs


class Dog(Animal):
    def speak(self) -> str:
        return "woof"


class Puppy(Dog):
    def speak(self) -> str:
        return "yip"

    def extra(self) -> str:
        return "puppy"


a: Animal = Animal()
d: Animal = Dog()
y: Animal = Puppy()
print("poly", a.describe(), d.describe(), y.describe(), d.leg_count())

# 覆盖不改变槽位：父类里写好的名字，子类实例上一样能调
ps: list[Animal] = [Animal(), Dog(), Puppy()]
for one in ps:
    print("iter", one.speak(), one.leg_count())


# ── 字段初值 ──────────────────────────────────────────────────────

class Defaults:
    a: int = 7
    b: str = "hi"

    def text(self) -> str:
        return self.b


class NoInit(Defaults):
    c: int = 9

    def __init__(self, c: int) -> None:
        self.c = c


n: NoInit = NoInit(42)
print("defaults", n.a, n.b, n.c, n.text())


# ── 8 个参数：self 占 rdi，后 5 个走栈 ────────────────────────────

class Big:
    a: int
    b: int
    c: int
    d: int
    e: int
    f: int
    g: int
    h: int

    def __init__(self, a: int, b: int, c: int, d: int, e: int, f: int, g: int, h: int) -> None:
        self.a = a
        self.b = b
        self.c = c
        self.d = d
        self.e = e
        self.f = f
        self.g = g
        self.h = h

    def total(self) -> int:
        return self.a + self.b + self.c + self.d + self.e + self.f + self.g + self.h

    def mix(self, p: Point) -> int:
        return self.total() + p.norm2()


bg: Big = Big(1, 2, 3, 4, 5, 6, 7, 8)
print("big", bg.total(), bg.mix(Point(1, 1)))


# ── 对象字段：链表 ────────────────────────────────────────────────

class Node:
    val: int
    next: "Node | None"

    def __init__(self, val: int) -> None:
        self.val = val
        self.next = None

    def total(self) -> int:
        acc: int = 0
        cur: Node | None = self
        while cur is not None:
            acc = acc + cur.val
            cur = cur.next
        return acc


n1: Node = Node(1)
n1.next = Node(2)
n1.next.next = Node(3)
print("list", n1.total())


# ── 默认参数与关键字参数 ──────────────────────────────────────────

class Vec:
    x: float
    y: float

    def __init__(self, x: float = 0.0, y: float = 0.0) -> None:
        self.x = x
        self.y = y

    def dot(self, o: Vec) -> float:
        return self.x * o.x + self.y * o.y


v1: Vec = Vec(1.5, 2.5)
v2: Vec = Vec(y=4.0)
print("vec", v1.x, v1.y, v2.x, v2.y, v1.dot(v2))


# ── 打印对象本身 ──────────────────────────────────────────────────

print(Point(0, 0))
print(Animal())

# ── 不写标注的字段：self.x = ... 就地长出来 ────────────────────────
# 标准库里绝大多数类都不写字段标注，这里钉住「先赋值后使用」也能跑。

class Lazy:
    def __init__(self, n: int) -> None:
        self.n = n
        self.label = "lazy"

    def bump(self) -> None:
        self.n = self.n + 1

lz: Lazy = Lazy(1)
lz.bump()
lz.bump()
print("implicit", lz.n, lz.label)

# --- expect ---
# point 3 4 25
# shift 4 5 41
# plus 14 25
# poly ... woof yip 4
# iter ... 4
# iter woof 4
# iter yip 4
# defaults 7 hi 42 hi
# big 36 38
# list 6
# vec 1.5 2.5 0.0 4.0 10.0
# <Point object>
# <Animal object>
# implicit 3 lazy
