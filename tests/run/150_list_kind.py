# 裸 list / 空字面量的元素种类：第一次写入时定型，冲突时降级为混合。
def push_ints(n: int) -> list:
    out = []
    i = 0
    while i < n:
        out.append(i * 2)
        i += 1
    return out

print(push_ints(4))

def push_strs() -> list:
    out: list = []
    out.append("a" + "b")
    out.append("c" + "d")
    return out

r = push_strs()
print(r[0], r[1])

def put(a: list, i: int, v: int) -> None:
    a[i] = v

xs = []
xs.append(1)
put(xs, 0, 7)
print(xs)

mixed = []
mixed.append(1)
mixed.append("a" + "b")
print(mixed[0], mixed[1])

floats = []
floats.append(1.5)
floats.append(2.5)
print(floats)

ins = []
ins.insert(0, "x" + "y")
ins.insert(0, "z" + "w")
print(ins[0], ins[1])

sl = []
sl[0:0] = ["p" + "q"]
print(sl[0])

ext = []
ext.extend(["k" + "1", "k2"])
ext.extend([3])
print(ext[0], ext[1], ext[2])

empty = []
cp = empty.copy()
cp.append("m" + "n")
sl2 = empty[:]
sl2.append("o" + "p")
print(cp[0], sl2[0])

src: list = []
src.append(1)
src.append(2)
sq = [x * x for x in src]
print(sq)

built = list()
built.append("q" + "r")
print(built[0])

nested = []
nested.append([1, 2])
nested.append([3])
print(len(nested))

pile = []
pile.append("e" + "f")
print(pile.pop())

def drain(a: list) -> None:
    while len(a) > 0:
        print(a.pop())

pile.append("g" + "h")
drain(pile)

def counter() -> None:
    acc = []
    def add(v: int) -> None:
        acc.append(v)
    add(1)
    add(2)
    print(acc)

counter()

# --- expect ---
# [0, 2, 4, 6]
# ab cd
# [7]
# 1 ab
# [1.5, 2.5]
# zw xy
# pq
# k1 k2 3
# mn op
# [1, 4]
# qr
# 2
# ef
# gh
# [1, 2]
