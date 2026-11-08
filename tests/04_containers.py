# 容器、切片、推导式、f-string
xs = [1, 2, 3, 4, 5]
ys = xs[1:3]
zs = xs[::-1]
head = xs[:2]
tail = xs[2:]
every_other = xs[::2]

matrix = [[1, 2], [3, 4]]
cell = matrix[0][1]

d = {"name": "py23", "version": 1}
d["extra"] = True
age = d.get("age", 0)

t = (1, "two", 3.0)
single = (1,)
empty = ()
nested = (1, (2, 3))

s = {1, 2, 3}

combined = [*xs, *ys]
merged = {**d, "new": 1}

name = "world"
n = 42
msg = f"hello {name}, n={n}, next={n + 1}, repr={name!r}, padded={n:>5}"
escaped = f"braces {{literal}} and {n * 2}"

lines = ["a", "b", "c"]
joined = ", ".join(lines)
upper = name.upper()
parts = "a,b,c".split(",")
stripped = "  pad  ".strip()

total = sum(v for v in xs if v > 2)

print(f"mean={total / len(xs):.2f}")
print(f"{'|'.join(str(v) for v in xs)}")
