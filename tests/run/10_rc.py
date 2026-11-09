# 引用计数：存储/别名/字段/列表/返回值。输出必须与 CPython 一致。
a: str = "alpha"
b: str = a
a = "beta"
print(b)

def make() -> str:
    s: str = "local"
    return s

def bang(x: str) -> str:
    return x + "!"

print(make())
print(bang("hi"))

class Box:
    s: str
    def __init__(self, s: str) -> None:
        self.s = s
    def get(self) -> str:
        return self.s

p = Box("one")
q = p
p = Box("two")
print(q.get())
print(p.get())
held: str = q.s
q = Box("three")
print(held)

l: list[str] = ["a", "b"]
m = l
l = ["c"]
print(m[0] + m[1])

items: list[str] = []
items.append("x" + "y")
items.append("z")
popped: str = items.pop()
print(popped)
print(items[0])

nums: list[int] = [1, 2]
nums[0] = nums[1]
print(nums[0])

words: list[str] = ["a", "b"]
words[0] = "x" + "y"
print(words[0])

class Bag:
    items: list[str]
    def __init__(self) -> None:
        self.items = []
    def add(self, s: str) -> None:
        self.items.append(s)
    def first(self) -> str:
        return self.items[0]

bag = Bag()
bag.add("hello")
alias = bag
bag = Bag()
print(alias.first())

x: str = "same"
x = x
print(x)

if "a" + "b":
    print("truthy")

i: int = 0
while ("a" + "b") and i < 3:
    i += 1
print(i)

tup: tuple[str, str] = ("x", "y")
copy = tup
tup = ("z", "w")
print(len(copy))
print(len(tup))

letters: str = ""
for ch in "abc":
    letters = letters + ch
print(letters)

# continue 会跳过 if 语句的清理点，循环水位必须兜底
w: int = 0
while w < 100:
    if ("a" + "b") and w >= 0:
        w += 1
        continue
    w += 1
print(w)

r: int = 0
for _i in range(100):
    if "x" + "y":
        continue
    r += 1
print(r)

seq: list[str] = ["a", "b", "c"]
hits: int = 0
for item in seq:
    if item + "!":
        hits += 1
        continue
print(hits)

s: str = ""
k: int = 0
while k < 1000:
    s = s + "x"
    k += 1
print(len(s))

total: str = ""
j: int = 0
while j < 50:
    total = total + f"{j},"
    j += 1
print(len(total))

# --- expect ---
# alpha
# local
# hi!
# one
# two
# one
# ab
# z
# xy
# 2
# xy
# hello
# same
# truthy
# 3
# 2
# 2
# abc
# 100
# 0
# 3
# 1000
# 140
