# random 的补充：randrange / getrandbits / gauss / expovariate / triangular /
# sample / choices。只验证分布性质（范围、均值、去重），不比对具体序列——
# 两边的高斯和指数分布用不同算法，序列本来就不同。
import random

random.seed(42)

bad1 = 0
i = 0
while i < 1000:
    v = random.randrange(10)
    if v < 0 or v >= 10:
        bad1 += 1
    i += 1
print("randrange1", bad1)

bad2 = 0
i = 0
while i < 1000:
    v = random.randrange(5, 15)
    if v < 5 or v >= 15:
        bad2 += 1
    i += 1
print("randrange2", bad2)

bad3 = 0
i = 0
while i < 1000:
    v = random.randrange(0, 20, 5)
    if v % 5 != 0 or v < 0 or v >= 20:
        bad3 += 1
    i += 1
print("randrange3", bad3)

bad4 = 0
i = 0
while i < 100:
    v = random.getrandbits(8)
    if v < 0 or v >= 256:
        bad4 += 1
    i += 1
print("getrandbits", bad4)

s = 0.0
i = 0
while i < 10000:
    g = random.gauss(10.0, 2.0)
    s += g
    i += 1
gm = s / 10000.0
print("gauss", gm > 9.5 and gm < 10.5)

bad5 = 0
s2 = 0.0
i = 0
while i < 5000:
    e = random.expovariate(0.5)
    if e <= 0:
        bad5 += 1
    s2 += e
    i += 1
em = s2 / 5000.0
print("expovariate", bad5, em > 1.5 and em < 2.5)

bad6 = 0
i = 0
while i < 1000:
    t = random.triangular(1.0, 5.0)
    if t < 1.0 or t > 5.0:
        bad6 += 1
    i += 1
print("triangular", bad6)

xs = [1, 2, 3, 4, 5, 6, 7, 8]
s1 = random.sample(xs, 3)
print("sample", len(s1), len(set(s1)))

c1 = random.choices(xs, k=5)
print("choices", len(c1))
# --- expect ---
# randrange1 0
# randrange2 0
# randrange3 0
# getrandbits 0
# gauss True
# expovariate 0 True
# triangular 0
# sample 3 3
# choices 5
