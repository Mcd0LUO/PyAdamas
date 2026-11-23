# 533_pysig_native_class.py —— .pysig 的原生类声明（class Random）
#
# random.Random 的方法表由 stdlib/sigs/random.pysig 的 `class Random:` 声明提供
# （构造器 __init__ + 6 个实例方法，@native 覆盖 pa_random_* 符号）。
# 期望值全部来自 CPython 3.x（MT19937 同种子逐位一致）。
import random

r = random.Random(42)
print("random42", r.random())
xs = [1, 2, 3, 4, 5]
print("randint", r.randint(1, 10))
print("uniform", r.uniform(1.0, 2.0))
print("choice", r.choice(xs))
r.shuffle(xs)
print("shuffle", xs)
r.seed(7)
print("reseed", r.random())
print("ctor42", random.Random(42).random())

from random import Random
r2 = Random(1)
print("from_import", r2.random())

# seed 缺省用 INT64_MIN 哨兵（与 CPython 的 None 语义近似：按时间/系统熵重置）
r.seed()
v = r.random()
print("seed_default", isinstance(v, float))

# 未知属性必须报 class 'Random' has no attribute（声明是唯一来源）
# 序列/数值参数的边界形态（与 CPython 同种子一致）
r = random.Random(7)
print("choice_str", r.choice("abc") in "abc")
print("choice_tuple", r.choice((10, 20)) in (10, 20))
print("uniform_int", r.uniform(1, 2) >= 1.0)
xs = [3, 1, 2]
r.shuffle(xs)
print("shuffle_int", sorted(xs) == [1, 2, 3])
r.seed(5)
print("seed_int", r.random() == random.Random(5).random())
print("noarg_ok", isinstance(random.Random().random(), float))
# --- expect ---
# random42 0.6394267984578837
# randint 1
# uniform 1.7415504997598328
# choice 2
# shuffle [4, 1, 3, 5, 2]
# reseed 0.32383276483316237
# ctor42 0.6394267984578837
# from_import 0.13436424411240122
# seed_default True
# choice_str True
# choice_tuple True
# uniform_int True
# shuffle_int True
# seed_int True
# noarg_ok True
