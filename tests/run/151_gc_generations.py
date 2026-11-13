# 分代 GC：新生代环、晋升后的老生代环、生成器帧与 unknown 局部槽里的环。

def make_cycle(n: int) -> list:
    a = []
    b = [a, n]
    a.append(b)
    return a

# 新生代环：外部引用消失后由 minor 回收。
i = 0
while i < 4000:
    make_cycle(i)
    i += 1

# 保持引用跨过一轮 minor（晋升为老生代），再丢弃，由 major 回收。
keep = []
i = 0
while i < 1000:
    keep.append(make_cycle(i))
    i += 1
i = 0
while i < 20000:
    tmp = ["y" * 32, str(i)]
    i += 1
keep = []

# 制造足够分配量触发 major。
i = 0
total = 0
while i < 60000:
    tmp = ["x" * 32, str(i)]
    total += len(tmp[1])
    i += 1
print(total)

# 生成器帧持有环：帧被回收时槽里的引用要正确释放（清扫阶段才清零）。
def gen_cycle() -> int:
    a = []
    b = [a]
    a.append(b)
    yield 1
    yield len(a)

g = gen_cycle()
print(next(g))
print(next(g))

# unknown 局部槽里的环：保守根只影响回收时机，不影响值。
def unknown_local() -> int:
    x: list = []
    y = [x, 1]
    x.append(y)
    return len(x)

print(unknown_local())

# --- expect ---
# 288890
# 1
# 1
# 1
