# 类/OOP 语义回归（0.432）：实例/类属性、继承与 super、isinstance/issubclass、
# 魔术方法（typed 域）、类作 dict/set 键、__add__/__len__/__bool__、生命周期。

# 1) 基本：实例字段、方法、类属性（普通赋值）、类属性读
class P:
    count = 0
    name = "p"

    def __init__(self, x: int) -> None:
        self.x = x
        self.y = x + 1

    def get(self) -> int:
        return self.x

    def add(self, k: int) -> int:
        return self.x + k


p1 = P(1)
p2 = P(2)
print("attrs", p1.x, p1.y, p2.x, p2.y)
print("methods", p1.get(), p2.add(10))
print("class_attr", P.count, p1.count, P.name, p2.name)
P.count = 5
print("class_write", P.count, p1.count)
p1.x = 100
print("field_write", p1.x, p2.x)

# 2) 继承 / 重写 / super / 动态分派 / isinstance / issubclass
class A:
    def __init__(self, v: int) -> None:
        self.v = v

    def name(self) -> str:
        return "A"

    def hello(self) -> str:
        return "hello-" + self.name()


class B(A):
    def __init__(self, v: int) -> None:
        super().__init__(v * 10)
        self.w = 1

    def name(self) -> str:
        return "B"

    def parent_hello(self) -> str:
        return super().hello()


b = B(2)
a = A(5)
print("inherit", b.v, b.w, b.name(), a.name())
print("dispatch", b.hello(), b.parent_hello(), a.hello())
print("isinstance", isinstance(b, A), isinstance(a, B), isinstance(a, A))
print("issubclass", issubclass(B, A), issubclass(A, B))

# 3) 魔术方法（静态类型域）
class V:
    def __init__(self, v: int) -> None:
        self.v = v

    def __eq__(self, other: "V") -> bool:
        return self.v == other.v

    def __ne__(self, other: "V") -> bool:
        return self.v != other.v

    def __lt__(self, other: "V") -> bool:
        return self.v < other.v

    def __len__(self) -> int:
        return self.v

    def __str__(self) -> str:
        return "V(" + str(self.v) + ")"

    def __repr__(self) -> str:
        return "V[" + str(self.v) + "]"

    def __bool__(self) -> bool:
        return self.v > 0

    def __add__(self, other: "V") -> "V":
        return V(self.v + other.v)


v1 = V(1)
v2 = V(1)
v3 = V(2)
print("eq", v1 == v2, v1 == v3, v1 != v3)
print("lt", v1 < v3, v3 < v1)
print("len", len(v1), len(v3))
print("str_repr", str(v1), v1, repr(v1), [v1, v3])
print("bool", bool(v1), bool(V(0)))
v4 = v1 + v3
print("add", v4.v, len(v4))

# 4) 类作 dict/set 键（__hash__ + __eq__ 走 vtable，不同实例内容相等即命中）
class K:
    def __init__(self, v: int) -> None:
        self.v = v

    def __hash__(self) -> int:
        return self.v

    def __eq__(self, other: "K") -> bool:
        return self.v == other.v


k1 = K(1)
k2 = K(1)
k3 = K(2)
d = {}
d[k1] = "one"
s = set([k1, k2, k3])
print("dict_key", d[k2], len(s))

# 5) 身份比较 / None 比较 / 删除后循环引用不崩
class N:
    def __init__(self, v: int) -> None:
        self.v = v
        self.next = None


n1 = N(1)
n2 = N(1)
print("identity", n1 == n2, n1 == n1)
print("none", n1 == None) if False else print("none", n1 is not None)
a1 = N(1)
b1 = N(2)
a1.next = b1
b1.next = a1
del a1
del b1
print("cycle_deleted")

# --- expect ---
# attrs 1 2 2 3
# methods 1 12
# class_attr 0 0 p p
# class_write 5 5
# field_write 100 2
# inherit 20 1 B A
# dispatch hello-B hello-B hello-A
# isinstance True False True
# issubclass True False
# eq True False True
# lt True False
# len 1 2
# str_repr V(1) V(1) V[1] [V[1], V[2]]
# bool True False
# add 3 3
# dict_key one 2
# identity False True
# none True
# cycle_deleted
