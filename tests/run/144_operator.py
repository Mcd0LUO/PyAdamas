from operator import itemgetter, truth, not_

g0 = itemgetter(0)
g1 = itemgetter(1)
print(g0([10, 20]))
print(g1(("a", "b")))
print(truth(0), truth(3))
print(not_(True), not_(False))

# --- expect ---
# 10
# b
# False True
# False True
