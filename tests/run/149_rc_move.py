# 新值所有权转移：存槽/返回/容器写入不能破坏别名和旧值。
s = "a" + "b"
t = s
s = "x" + "y"
print(t, s)

a = "m" + "n"
b = a
a = "p" + "q"
print(b, a)

def make() -> str:
    return "r" + "s"

print(make())

l: list[str] = []
l.append("p" + "q")
l.append("r" + "s")
print(l[0], l[1])

l2: list[str] = ["old"]
l2[0] = "b" + "c"
print(l2[0])

d: dict[str, str] = {}
d["k" + "1"] = "v" + "1"
d["k2"] = "v" + "2"
print(d["k1"], d["k2"])

class Box:
    s: str
    def __init__(self, v: str) -> None:
        self.s = v

box = Box("old")
box.s = "n" + "e" + "w"
print(box.s)

xs = ["a" + "b", "c" + "d"]
ys = xs
xs = ["e" + "f"]
print(ys[0], ys[1], xs[0])

n = 0
total = ""
while n < 100:
    total = total + "x"
    n += 1
print(len(total))

# --- expect ---
# ab xy
# mn pq
# rs
# pq rs
# bc
# v1 v2
# new
# ab cd ef
# 100
