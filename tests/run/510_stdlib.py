# stdlib 文档承诺审计回归（0.443）：math / json / random / os.path / datetime。
import math
import json
import random
import os.path as osp
import datetime

print("sqrt", math.sqrt(9.0), math.sqrt(2.0))
print("floorceil", math.floor(2.7), math.ceil(2.1), math.floor(-2.7), math.ceil(-2.1))
print("trig", math.sin(0.0), math.cos(0.0), math.atan(1.0))
print("logexp", math.log(math.e), math.log2(8.0), math.log10(100.0), math.exp(0.0), math.exp2(3.0))
print("powmod", math.pow(2.0, 10.0), math.fmod(7.0, 3.0), math.remainder(7.0, 3.0))
print("deg", math.degrees(math.pi), math.radians(180.0), math.trunc(2.9), math.trunc(-2.9))
print("next", math.nextafter(1.0, 2.0), math.ulp(1.0), math.ldexp(1.0, 3), math.fma(2.0, 3.0, 4.0))
print("pred", math.isnan(math.nan), math.isinf(math.inf), math.isfinite(1.0), math.isclose(1.0, 1.0000000001))
print("erf", math.erf(1.0), math.erfc(0.0), math.gamma(5.0), math.lgamma(1.0))
print("intf", math.factorial(10), math.gcd(12, 18), math.lcm(4, 6), math.comb(5, 2), math.perm(5, 2), math.isqrt(17))
print("fsum", math.fsum([0.1, 0.2, 0.3]), math.prod([2, 3, 4]), math.sumprod([1, 2], [3, 4]))
print("dist", math.dist([0.0, 0.0], [3.0, 4.0]))
fx, fe = math.frexp(8.0)
mf, mi = math.modf(2.5)
print("frexp_modf", fx, fe, mf, mi)
print("consts", math.pi > 3.14 and math.pi < 3.15, math.e > 2.71 and math.e < 2.72)
di = {"a": 1, "b": 2}
print("jdumps", json.dumps(di), json.dumps([1, 2, 3]), json.dumps("hi"), json.dumps(True))
print("jdumps_f", json.dumps(1.5))
random.seed(42)
print("rand", random.random(), random.random(), random.random())
random.seed(42)
print("randint", random.randint(1, 100), random.randint(1, 100), random.randint(-5, 5))
random.seed(7)
print("choice", random.choice(["a", "b", "c", "d"]), random.choice(["a", "b", "c", "d"]))
random.seed(7)
nums = [1, 2, 3, 4, 5]
random.shuffle(nums)
print("shuffle", nums)
print("ospath", osp.join("a", "b"), osp.join("a", "b", "c"), osp.join("a/", "b"))
print("osexists", osp.exists("/tmp"), osp.exists("/definitely/not/there"))
print("osisfile", osp.isfile("/etc/passwd"))
d = datetime.date(2024, 2, 29)
print("date", d.year, d.month, d.day, d.weekday(), d.isoformat())
dt = datetime.datetime(2024, 2, 29, 13, 45, 30)
print("dt", dt.hour, dt.minute, dt.second, dt.strftime("%H:%M"))
td = datetime.timedelta(days=2, seconds=30)
print("td", td.days, td.seconds, td.total_seconds())
print("date_add", (d + datetime.timedelta(days=1)).isoformat())
rep = 0
try:
    d.replace(year=2025)
except ValueError:
    rep = 1
print("date_replace_err", rep)
print("date_replace_ok", datetime.date(2023, 2, 28).replace(year=2024).isoformat())

# --- expect ---
# sqrt 3.0 1.4142135623730951
# floorceil 2 3 -3 -2
# trig 0.0 1.0 0.7853981633974483
# logexp 1.0 3.0 2.0 1.0 8.0
# powmod 1024.0 1.0 1.0
# deg 180.0 3.141592653589793 2 -2
# next 1.0000000000000002 2.220446049250313e-16 8.0 10.0
# pred True True True True
# erf 0.8427007929497149 1.0 24.0 0.0
# intf 3628800 6 12 10 20 4
# fsum 0.6 24 11
# dist 5.0
# frexp_modf 0.5 4 0.5 2.0
# consts True True
# jdumps {"a": 1, "b": 2} [1, 2, 3] "hi" true
# jdumps_f 1.5
# rand 0.6394267984578837 0.025010755222666936 0.27502931836911926
# randint 82 15 -5
# choice c b
# shuffle [5, 1, 4, 2, 3]
# ospath a/b a/b/c a/b
# osexists True False
# osisfile True
# date 2024 2 29 3 2024-02-29
# dt 13 45 30 13:45
# td 2 30 172830.0
# date_add 2024-03-01
# date_replace_err 1
# date_replace_ok 2024-02-28
