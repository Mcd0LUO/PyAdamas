# 推导式与作用域语义回归（0.439）：多 for/过滤/嵌套、作用域不泄漏、闭包引用捕获、
# global/nonlocal、默认参数、生成器表达式惰性。
a = [1, 2, 3]
b = [10, 20]
print("multi", [x * y for x in a for y in b])
print("multi_filter", [x + y for x in a for y in b if x + y > 21])
print("filters", [x for x in range(10) if x > 2 if x % 2 == 0])
m = [[1, 2], [3, 4]]
print("nested", [[y * 10 for y in row] for row in m])
print("flat", [v for row in m for v in row])
base = 5
print("outer_ref", [x + base for x in a])
print("dict_comp", {k: k * 2 for k in ["a", "b"]})
print("set_comp", sorted([x % 3 for x in range(7)]))
print("dict_zip", {k: v for k, v in zip(["a", "b"], [1, 2])})
x = 100
comp = [x for x in range(3)]
print("no_leak", comp, x)
def scope_fn() -> int:
    v = 7
    inner = [v for v in range(3)]
    return v + inner[0]


print("shadow", scope_fn())
print("sum_list", sum([i * i for i in a]))
print("sum_gen", sum(i * i for i in a))
g = (i for i in range(3))
print("genexpr_lazy", list(g), list(g))
def make_adder(n: int):
    def add(x: int) -> int:
        return x + n
    return add


print("closure", make_adder(3)(1), make_adder(5)(1))
def by_ref() -> int:
    v = 1

    def read() -> int:
        return v

    v = 2
    return read()


print("by_ref", by_ref())
fs = []
for i in range(3):
    def g2() -> int:
        return i
    fs.append(g2)
print("late_bind", [f() for f in fs])
def counter():
    n = 0

    def inc() -> int:
        nonlocal n
        n = n + 1
        return n

    return inc


c1 = counter()
c2 = counter()
print("nonlocal", c1(), c1(), c2())
count = 0
def bump() -> int:
    global count
    count = count + 1
    return count


print("global", bump(), bump(), count)
def default_args(p: int, q: int = 10) -> int:
    return p + q


print("default", default_args(1), default_args(1, 2))
def three() -> int:
    aa = 1

    def mid() -> int:
        bb = 2

        def inner() -> int:
            return aa + bb

        return inner()

    return mid()


print("three_level", three())

# --- expect ---
# multi [10, 20, 20, 40, 30, 60]
# multi_filter [22, 23]
# filters [4, 6, 8]
# nested [[10, 20], [30, 40]]
# flat [1, 2, 3, 4]
# outer_ref [6, 7, 8]
# dict_comp {'a': 'aa', 'b': 'bb'}
# set_comp [0, 0, 0, 1, 1, 2, 2]
# dict_zip {'a': 1, 'b': 2}
# no_leak [0, 1, 2] 100
# shadow 7
# sum_list 14
# sum_gen 14
# genexpr_lazy [0, 1, 2] []
# closure 4 6
# by_ref 2
# late_bind [2, 2, 2]
# nonlocal 1 2 1
# global 1 2 2
# default 11 3
# three_level 3
