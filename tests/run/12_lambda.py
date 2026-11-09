# lambda：脱糖成嵌套 def，捕获/默认参数/嵌套/立即调用/高阶传参
inc = lambda x: x + 1
print(inc(2))
print((lambda x: x * 2)(5))

def make(k: int):
    return lambda x: x + k

a = make(10)
b = make(20)
print(a(1))
print(b(1))

g = lambda x, y=3: x + y
print(g(2))
print(g(2, 4))

h = lambda x: (lambda y: x + y)
print(h(10)(5))

class C:
    def run(self):
        return lambda x: x + 1
print(C().run()(7))

fns = [lambda: 1, lambda: 2]
print(fns[0]())
print(fns[1]())

def apply(fn, x: int) -> int:
    return fn(x)

print(apply(lambda v: v * v, 6))

def make_late():
    n: int = 0
    get = lambda: n
    n = 7
    return get

print(make_late()())

# --- expect ---
# 3
# 10
# 11
# 21
# 5
# 6
# 15
# 8
# 1
# 2
# 36
# 7
