xs = [3, 1, 2]
print(sorted(xs)[0], sorted(xs)[-1], xs[0])
print(list(reversed([1, 2, 3]))[0])
for i, v in enumerate(["a", "b"]):
    print(i, v)
for a, b in zip([1, 2], [10, 20, 30]):
    print(a, b)
ys = list()
ys.append(5)
print(len(ys))
print(list([1, 2])[0], list("ab")[1], len(tuple([3, 4])))
d = dict()
d[1] = 2
print(len(d))
d2 = dict(a=1, b=2)
print(len(d2), d2["a"], d2["b"])
s = set([1, 2, 2])
print(len(s), 1 in s)
s2 = set()
s2.add(3)
print(len(s2))
print(sorted(["b", "a"])[0])
print(sorted([2.5, 1.5])[0])
print(len(set("aab")))

# --- expect ---
# 1 3 3
# 3
# 0 a
# 1 b
# 1 10
# 2 20
# 1
# 1 b 2
# 1
# 2 1 2
# 2 True
# 1
# a
# 1.5
# 2
