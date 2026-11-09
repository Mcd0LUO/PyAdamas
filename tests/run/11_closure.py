# 闭包：捕获、nonlocal、多实例、多层嵌套、自递归、方法里的 self
def make_counter():
    n: int = 0
    def inc() -> int:
        nonlocal n
        n = n + 1
        return n
    return inc

f = make_counter()
g = make_counter()
print(f())
print(f())
print(g())

def make_adder(k: int):
    def add(x: int) -> int:
        return x + k
    return add

a = make_adder(10)
b = make_adder(20)
print(a(1))
print(b(1))

def make_greeter(name: str):
    def hi() -> str:
        return "hello, " + name
    return hi

print(make_greeter("py23")())

def make_acc():
    s: str = ""
    def add(x: str) -> str:
        nonlocal s
        s = s + x
        return s
    return add

acc = make_acc()
print(acc("a"))
print(acc("b"))

def outer():
    n: int = 0
    def mid():
        def inner() -> int:
            nonlocal n
            n = n + 1
            return n
        return inner
    return mid

m = outer()
i1 = m()
i2 = m()
print(i1())
print(i2())

def make_fact():
    def fact(n: int) -> int:
        if n <= 1:
            return 1
        return n * fact(n - 1)
    return fact

fac = make_fact()
print(fac(5))

class Counter:
    n: int
    def __init__(self) -> None:
        self.n = 0
    def maker(self):
        def bump() -> int:
            self.n = self.n + 1
            return self.n
        return bump

c = Counter()
bump = c.maker()
print(bump())
print(bump())

def make_value(v: int):
    def get() -> int:
        return v
    return get

fs = [make_value(1), make_value(2)]
print(fs[0]())
print(fs[1]())

def apply(fn, x: int) -> int:
    return fn(x)

def square(x: int) -> int:
    return x * x

def plus_one(x: int) -> int:
    return x + 1

print(apply(square, 7))

ops = [square, plus_one]
ops[0] = plus_one
print(ops[0](7))

def make_sum(base: int):
    def rec(n: int) -> int:
        if n <= 0:
            return base
        return base + rec(n - 1)
    return rec

print(make_sum(10)(3))

def late():
    n: int = 0
    def get() -> int:
        return n
    n = 5
    return get

print(late()())

def make_list_counter():
    box: list[int] = [0]
    def bump() -> int:
        box[0] = box[0] + 1
        return box[0]
    return bump

lb = make_list_counter()
print(lb())
print(lb())

counter: int = 0
def bump_global() -> None:
    global counter
    counter = counter + 1
bump_global()
bump_global()
print(counter)

def pair():
    n: int = 0
    def inc() -> int:
        nonlocal n
        n = n + 1
        return n
    def get() -> int:
        return n
    return [inc, get]

p = pair()
print(p[0]())
print(p[1]())
print(p[0]())
print(p[1]())

# --- expect ---
# 1
# 2
# 1
# 11
# 21
# hello, py23
# a
# ab
# 1
# 2
# 120
# 1
# 2
# 1
# 2
# 49
# 8
# 40
# 5
# 1
# 2
# 2
# 1
# 1
# 2
# 2
