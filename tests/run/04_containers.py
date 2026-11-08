# 列表、元组、索引、解包、遍历

def total(xs: list[int]) -> int:
    s = 0
    for x in xs:
        s += x
    return s

xs: list[int] = [1, 2, 3]
xs.append(10)
print("len =", len(xs))
print("total =", total(xs))
print(xs[1])
xs[0] = 100
print(xs[0])

for v in [1, 2, 3]:
    print(v * v)

a, b = 1, 2
a, b = b, a
print(a, b)
(x, y) = (10, 20)
print(x, y)

def bubble(xs: list[int], n: int) -> None:
    for i in range(n):
        for j in range(n - i - 1):
            if xs[j] > xs[j + 1]:
                t = xs[j]
                xs[j] = xs[j + 1]
                xs[j + 1] = t

ys = [5, 2, 9, 1, 7]
bubble(ys, len(ys))
ok = True
for i in range(len(ys) - 1):
    if ys[i] > ys[i + 1]:
        ok = False
print("sorted", ok)

# --- expect ---
# len = 4
# total = 16
# 2
# 100
# 1
# 4
# 9
# 2 1
# 10 20
# sorted True
