# 532_pysig_ret_specialized.py —— .pysig 的两个声明能力缺口回归
#
# 1) @ret_specialized：返回类型由调用点按实参特化（ret_known=false），
#    random.choice/sample/choices 的声明与硬编码语义一致；
# 2) 字面量表达力：seed 的 INT64_MIN 哨兵默认值、math.nan = float("nan")。
import math
import random

# math.nan：声明值与实际值一致（真 NaN，不是 +inf）
print("nan_isnan", math.isnan(math.nan))
print("nan_ne_self", math.nan != math.nan)
print("inf_isinf", math.isinf(math.inf), math.inf > 0)

# seed 缺省参数 = INT64_MIN（无参调用可编译且确定）
random.seed()
random.seed(1234)
a = random.random()
random.seed(1234)
b = random.random()
print("seed_default_ok", a == b)

# @ret_specialized：choice 的返回类型是实参元素类型（可赋给标注变量）
xs = [10, 20, 30]
v: int = random.choice(xs)
print("choice_int", v in xs)
s: str = random.choice(["a", "bb"])
print("choice_str", s in ["a", "bb"])

# sample / choices 仍返回 list（元素类型按实参）
l = random.sample(xs, 2)
print("sample", len(l) == 2, l[0] in xs)
c = random.choices(xs, k=3)
print("choices", len(c) == 3, c[0] in xs)
# --- expect ---
# nan_isnan True
# nan_ne_self True
# inf_isinf True True
# seed_default_ok True
# choice_int True
# choice_str True
# sample True True
# choices True True
