# int(float) 的域检查（0.426）：NaN → ValueError、±inf → OverflowError；
# 合法的 -2^63 不能误抛；正常截断与负向截断回归。

inf = float("inf")
nan = inf - inf

try:
    x = int(inf)
    print("int_inf", x)
except OverflowError as e:
    print("int_inf_overflow", str(e))

try:
    x = int(-inf)
    print("int_ninf", x)
except OverflowError as e:
    print("int_ninf_overflow", str(e))

try:
    x = int(nan)
    print("int_nan", x)
except ValueError as e:
    print("int_nan_value", str(e))

# float(-2^63) 精确可表示：int() 必须得到 MIN，不能当成哨兵误抛
MIN = -9223372036854775807 - 1
print("int_min_float", int(float(MIN)))
print("min_is_int", int(float(MIN)) == MIN)

# 正常截断（向零）与边界
print("trunc", int(2.9), int(-2.9), int(0.5), int(-0.5), int(-0.0))
print("big_f", int(float(2 ** 53)), int(float(2 ** 62)))
print("nan_after", nan != nan)

# 7) math.floor/ceil/trunc 的同类检查（CPython 也抛同样的异常）
import math

try:
    x = math.floor(inf)
    print("floor_inf", x)
except OverflowError as e:
    print("floor_inf_overflow", str(e))

try:
    x = math.floor(nan)
    print("floor_nan", x)
except ValueError as e:
    print("floor_nan_value", str(e))

try:
    x = math.ceil(-inf)
    print("ceil_ninf", x)
except OverflowError as e:
    print("ceil_ninf_overflow", str(e))

try:
    x = math.trunc(nan)
    print("trunc_nan", x)
except ValueError as e:
    print("trunc_nan_value", str(e))

print("math_ok", math.floor(2.9), math.floor(-2.9), math.ceil(2.1), math.trunc(-2.9))
print("floor_min", math.floor(float(MIN)))

# --- expect ---
# int_inf_overflow cannot convert float infinity to integer
# int_ninf_overflow cannot convert float infinity to integer
# int_nan_value cannot convert float NaN to integer
# int_min_float -9223372036854775808
# min_is_int True
# trunc 2 -2 0 0 0
# big_f 9007199254740992 4611686018427387904
# nan_after True
# floor_inf_overflow cannot convert float infinity to integer
# floor_nan_value cannot convert float NaN to integer
# ceil_ninf_overflow cannot convert float infinity to integer
# trunc_nan_value cannot convert float NaN to integer
# math_ok 2 -3 3 -2
# floor_min -9223372036854775808
