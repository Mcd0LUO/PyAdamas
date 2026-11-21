# 增量赋值把目标加宽成 float（int += float）：typecheck 把变量/元素/字段
# 的槽加宽成 float，但目标表达式的缓存类型还是 int。降级期按 int 读原值：
# 变量得到 9.2e18 这种垃圾 double，容器元素读到 1.0 的位模式
# 4607182418800017408（4.6e18），-O2 汇编还会失败（operand type mismatch）。

class Box:
    def __init__(self) -> None:
        self.v = 1

    def bump(self) -> float:
        self.v += 0.5
        return self.v

def local() -> float:
    x = 1
    x += 0.5
    return x

def list_elem() -> float:
    xs = [1]
    xs[0] += 0.5
    return xs[0]

def dict_elem() -> float:
    d = {1: 1}
    d[1] += 0.5
    return d[1]

g = 1

def global_elem() -> float:
    global g
    g += 0.5
    return g

print(local())
print(list_elem())
print(dict_elem())
print(global_elem())
print(Box().bump())
x = 1
x -= 0.5
print(x)
xs = [2]
xs[0] *= 1.5
print(xs[0])
# --- expect ---
# 1.5
# 1.5
# 1.5
# 1.5
# 1.5
# 0.5
# 3.0
