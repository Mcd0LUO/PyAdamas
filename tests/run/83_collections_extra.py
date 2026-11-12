# collections：OrderedDict（py23 的 dict 本来就保序）+ Counter 的既有行为
import collections

d = collections.OrderedDict()
d["b"] = 1
d["a"] = 2
print(len(d))
print(d["a"], d["b"])

c = collections.Counter([1, 1, 2, 3, 3, 3])
print(c[1], c[3], c[2])
print(c.most_common(2))
# --- expect ---
# 2
# 2 1
# 2 3 1
# [(3, 3), (1, 2)]
