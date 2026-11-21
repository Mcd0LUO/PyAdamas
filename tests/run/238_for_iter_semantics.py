# for 迭代语义：迭代器绑在入口对象上、每轮查当前长度（CPython 口径）
# 锁住 95ad9f5（循环体可能改长度 → 每轮重读 len）：
#   * 快照迭代（xs[:]）看不到 append 的新元素；
#   * 直接迭代会看到 append 的新元素（有界写法避免无限增长）；
#   * 循环体里重绑迭代名只改局部名，不影响正在迭代的列表。

# 1) 快照语义：append 不会进入本轮迭代
xs = [1, 2, 3]
seen: list[int] = []
for x in xs[:]:
    seen.append(x)
    xs.append(x * 10)
print(seen, xs)

# 2) 直接迭代：append 的新元素会被继续迭代（只追加 2 次，保证有界）
b = [1, 2, 3]
n = 0
total = 0
for x in b:
    if n < 2:
        b.append(x + 10)
    n += 1
    total += x
print(total, b)

# 3) 重绑迭代名：迭代器仍绑在入口对象上
def rebind(xs: list[int]) -> int:
    s = 0
    for x in xs:
        xs = [1]
        s += x
    return s
print(rebind([1, 2, 3]))

# 4) 重绑成切片
def rebind_slice(xs: list[int]) -> int:
    s = 0
    for x in xs:
        xs = xs[1:]
        s += x
    return s
print(rebind_slice([1, 2, 3]))

# 5) 重绑成空列表
def rebind_empty(xs: list[int]) -> int:
    s = 0
    for x in xs:
        xs = []
        s += x
    return s
print(rebind_empty([4, 5]))

# 6) 体内调函数追加（追加发生在被迭代的列表上），有界
def add(x: int, ys: list[int]) -> int:
    if len(ys) < 6:
        ys.append(x * 2)
    return x

def call_mutate(zs: list[int]) -> int:
    t = 0
    for x in zs:
        t += add(x, zs)
    return t
print(call_mutate([1, 2, 3]))

# 7) 体内 pop：迭代器提前结束，不越界
p = [1, 2, 3, 4]
cnt = 0
for x in p:
    p.pop()
    cnt += 1
print(cnt, p)

# 8) 空列表上迭代：体内 append 的新元素不会被看到（迭代器一开始就结束）
def grow_from_empty(es: list[int]) -> int:
    k = 0
    for x in es:
        es.append(1)
        k += 1
    return k
print(grow_from_empty([]))

# --- expect ---
# [1, 2, 3] [1, 2, 3, 10, 20, 30]
# 29 [1, 2, 3, 11, 12]
# 6
# 6
# 9
# 18
# 2 [1, 2]
# 0
