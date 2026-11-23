# stdlib/sigs/random.pysig 迁移回归：模块级函数 + 保留的成员
# 随机值用范围/类型判定，与 CPython 语义对拍（不逐值比较）。
import random

random.seed(42)
print("r", isinstance(random.random(), float))
print("ri", 1 <= random.randint(1, 10) <= 10)
print("u", 1.0 <= random.uniform(1.0, 2.0) <= 2.0)
print("rb", random.getrandbits(8) >= 0)
print("g", isinstance(random.gauss(0.0, 1.0), float))
print("e", random.expovariate(1.0) >= 0)
print("t", isinstance(random.triangular(0.0, 1.0), float))
xs = [1, 2, 3, 4, 5]
print("c", random.choice(xs) in xs)
print("s", len(random.sample(xs, 2)) == 2)
print("ch", len(random.choices(xs, k=3)) == 3)
print("rr", 1 <= random.randrange(1, 10) < 10)
random.shuffle(xs)
print("sh", sorted(xs) == [1, 2, 3, 4, 5])

# Random 类（保留在 C++）
r = random.Random(1)
print("cls", isinstance(r.random(), float))

# from import
from random import randint, getrandbits
print("from", 1 <= randint(1, 10) <= 10, getrandbits(4) >= 0)

# --- expect ---
# r True
# ri True
# u True
# rb True
# g True
# e True
# t True
# c True
# s True
# ch True
# rr True
# sh True
# cls True
# from True True
