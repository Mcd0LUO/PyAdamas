# 装饰器：返回值替换、从下到上叠加、闭包捕获、局部函数
def deco(f):
    def wrapper(a: int, b: int) -> int:
        return f(a, b) + 10
    return wrapper

def twice(f):
    def wrapper(a: int, b: int) -> int:
        return f(a, b) * 2
    return wrapper

@deco
def add(a: int, b: int) -> int:
    return a + b

@deco
@twice
def mul(a: int, b: int) -> int:
    return a * b

print(add(2, 3))
print(mul(2, 3))

def outer(base: int) -> int:
    @deco
    def local(a: int, b: int) -> int:
        return a + b + base
    return local(1, 1)

print(outer(5))

# unknown 捕获的保守 retain：c 被重新赋值后，cell 里的旧闭包仍要活着
def make_caller(f):
    def caller(a: int, b: int) -> int:
        return f(a, b)
    return caller

def make_adder(k: int):
    def inner(x: int, y: int) -> int:
        return x + y + k
    return inner

c = make_adder(10)
caller = make_caller(c)
c = make_adder(20)
print(caller(1, 2))
# --- expect ---
# 15
# 22
# 17
# 13
