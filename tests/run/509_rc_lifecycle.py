# 对象生命周期小规模回归（0.442）：容器/环/元素替换/临时对象/实例/闭包/生成器丢弃。
a: list = []
for i in range(500):
    inner: list = []
    for k in range(3):
        inner.append("s" + str(i) + "-" + str(k))
    a.append(inner)
print("nested", len(a), len(a[0]))
a.clear()
for r in range(2000):
    cyc: list = []
    b: list = [cyc]
    cyc.append(b)
print("cycles", 1)
base: list = []
for i in range(100):
    base.append("e" + str(i))
for r in range(500):
    base[0] = "new" + str(r)
    base[50] = "mid" + str(r)
    del base[0]
    base.append("tail" + str(r))
print("replace", len(base))
base.clear()
print("cleared", len(base))
acc = ""
for i in range(1000):
    acc = "p" + str(i % 50)
print("concat", len(acc))
cnt = 0
for i in range(500):
    try:
        raise ValueError("m" + str(i % 5))
    except ValueError:
        cnt = cnt + 1
print("exceptions", cnt)
class Item:
    v: int

    def __init__(self, v: int) -> None:
        self.v = v

    def get(self) -> int:
        return self.v


total = 0
for i in range(1000):
    it = Item(i)
    total = total + it.get()
print("instances", total)
def make_adder(n: int):
    calls: list = []

    def add(x: int) -> int:
        calls.append(x)
        return x + n

    return add


sumv = 0
for i in range(500):
    f = make_adder(i)
    sumv = sumv + f(1)
print("closures", sumv)
def gen(n: int):
    for i in range(n):
        yield i


dropped = 0
for i in range(500):
    g = gen(10)
    v = next(g)
    dropped = dropped + v
print("gen_drop", dropped)

# --- expect ---
# nested 500 3
# cycles 1
# replace 100
# cleared 0
# concat 3
# exceptions 500
# instances 499500
# closures 125250
# gen_drop 0
