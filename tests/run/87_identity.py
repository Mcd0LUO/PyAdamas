# is / is not 是同一性比较：比指针，不比内容
s = "abc"
print(s is None)
print(s is not None)

xs = [1, 2]
ys = xs
zs = [1, 2]
print(xs is ys)
print(xs is zs)
print(xs is not zs)

t = None
print(t is None)

n = 5
print(n is None)
print(n is not None)

d = {"k": 1}
print(d is None)
print(d is d)

empty = ""
print(empty is None)
# --- expect ---
# False
# True
# True
# False
# True
# True
# False
# True
# False
# True
# False
