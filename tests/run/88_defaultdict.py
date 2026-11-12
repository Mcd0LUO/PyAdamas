# collections.defaultdict：缺省下标读取按工厂造值并插入
import collections
from collections import defaultdict

d = collections.defaultdict(int)
d["a"] += 1
d["b"] += 2
print(d["a"], d["b"], len(d))

e = collections.defaultdict(list)
e["x"].append(3)
e["x"].append(4)
e["y"].append(5)
print(e["x"], e["y"], len(e))

s = collections.defaultdict(str)
s["k"] += "v"
print(s["k"], len(s))

n = collections.defaultdict(None)
n["k"] = 1
print(n["k"], len(n))

c = collections.defaultdict(int, {"q": 9})
print(c["q"], c["w"], len(c))

# get / in 不触发工厂
g = collections.defaultdict(int)
miss = g.get("missing")
print(len(g), "missing" in g, miss is None)
x = g["made"]
print(x, len(g))

# copy 保留工厂
h = c.copy()
print(h["z"], len(h))

f = defaultdict(float)
f["v"] += 1.5
print(f["v"])
# --- expect ---
# 1 2 2
# [3, 4] [5] 2
# v 1
# 1 1
# 9 0 2
# 0 False True
# 0 1
# 0 3
# 1.5
