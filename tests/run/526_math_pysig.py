# stdlib/sigs/math.pysig 迁移回归：全部 57 个函数 + 5 个常量
# 与 CPython 3.14 同形脚本逐行对拍（浮点格式差异如实记录）。
import math

# 常量
print("pi", math.pi)
print("e", math.e)
print("tau", math.tau)
print("inf_pos", math.inf > 0)
print("inf_neg", -math.inf < 0)
print("nan", math.isnan(math.nan))

# 单参数 float -> float
print("sqrt", math.sqrt(9.0))
print("fabs", math.fabs(-2.5))
print("sin", round(math.sin(0.0), 6))
print("cos", round(math.cos(0.0), 6))
print("tan", round(math.tan(0.0), 6))
print("log", round(math.log(1.0), 6))
print("log2", round(math.log2(8.0), 6))
print("log10", round(math.log10(100.0), 6))
print("exp", round(math.exp(0.0), 6))
print("degrees", round(math.degrees(3.141592653589793), 6))
print("radians", round(math.radians(180.0), 6))
print("acos", round(math.acos(1.0), 6))
print("asin", round(math.asin(0.0), 6))
print("atan", round(math.atan(0.0), 6))
print("acosh", round(math.acosh(1.0), 6))
print("asinh", round(math.asinh(0.0), 6))
print("atanh", round(math.atanh(0.0), 6))
print("cbrt", round(math.cbrt(27.0), 6))
print("cosh", round(math.cosh(0.0), 6))
print("sinh", round(math.sinh(0.0), 6))
print("tanh", round(math.tanh(0.0), 6))
print("exp2", round(math.exp2(3.0), 6))
print("expm1", round(math.expm1(0.0), 6))
print("log1p", round(math.log1p(0.0), 6))
print("erf", round(math.erf(0.0), 6))
print("erfc", round(math.erfc(0.0), 6))
print("gamma", round(math.gamma(5.0), 6))
print("lgamma", round(math.lgamma(5.0), 6))
print("ulp", math.ulp(1.0))

# 单参数 float -> int
print("floor", math.floor(3.7))
print("ceil", math.ceil(3.2))
print("trunc", math.trunc(-3.7))

# 单参数 float -> bool
print("isnan", math.isnan(float("nan")), math.isnan(1.0))
print("isinf", math.isinf(float("inf")), math.isinf(1.0))
print("isfinite", math.isfinite(1.0), math.isfinite(float("inf")))

# 双参数 float -> float
print("pow", math.pow(2.0, 10.0))
print("fmod", math.fmod(7.0, 3.0))
print("atan2", round(math.atan2(0.0, 1.0), 6))
print("hypot", math.hypot(3.0, 4.0))
print("copysign", math.copysign(1.0, -2.0))
print("nextafter", math.nextafter(1.0, 2.0))
print("remainder", math.remainder(7.0, 3.0))

# 整数函数
print("factorial", math.factorial(10))
print("isqrt", math.isqrt(50))
print("gcd", math.gcd(12, 18))
print("lcm", math.lcm(4, 6))
print("comb", math.comb(10, 3))
print("perm", math.perm(10, 3))

# 容器 / 混合参数
print("fsum", math.fsum([1.0, 2.0, 3.0]))
print("ldexp", math.ldexp(1.0, 10))
print("fma", math.fma(2.0, 3.0, 4.0))
print("isclose", math.isclose(1.0, 1.0000001), math.isclose(1.0, 1.1))
print("frexp", math.frexp(8.0))
print("modf", math.modf(3.75))
print("prod", math.prod([1, 2, 3, 4]))
print("sumprod", math.sumprod([1, 2, 3], [4, 5, 6]))
print("dist", math.dist([0.0, 0.0], [3.0, 4.0]))

# --- expect ---
# pi 3.141592653589793
# e 2.718281828459045
# tau 6.283185307179586
# inf_pos True
# inf_neg True
# nan True
# sqrt 3.0
# fabs 2.5
# sin 0.0
# cos 1.0
# tan 0.0
# log 0.0
# log2 3.0
# log10 2.0
# exp 1.0
# degrees 180.0
# radians 3.141593
# acos 0.0
# asin 0.0
# atan 0.0
# acosh 0.0
# asinh 0.0
# atanh 0.0
# cbrt 3.0
# cosh 1.0
# sinh 0.0
# tanh 0.0
# exp2 8.0
# expm1 0.0
# log1p 0.0
# erf 0.0
# erfc 1.0
# gamma 24.0
# lgamma 3.178054
# ulp 2.220446049250313e-16
# floor 3
# ceil 4
# trunc -3
# isnan True False
# isinf True False
# isfinite True False
# pow 1024.0
# fmod 1.0
# atan2 0.0
# hypot 5.0
# copysign -1.0
# nextafter 1.0000000000000002
# remainder 1.0
# factorial 3628800
# isqrt 7
# gcd 6
# lcm 12
# comb 120
# perm 720
# fsum 6.0
# ldexp 1024.0
# fma 10.0
# isclose False False
# frexp (0.5, 4)
# modf (0.75, 3.0)
# prod 24
# sumprod 32
# dist 5.0
