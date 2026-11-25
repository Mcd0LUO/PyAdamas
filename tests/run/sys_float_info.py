# sys.float_info / sys.flags（0.520 B 部分）。与 CPython 3.14.4 逐字节一致：
# 三个 float 属性是编译期常量（DBL_*），整数属性走原生 getter，flags 全 0。
import sys

print(sys.float_info.epsilon)
print(sys.float_info.max)
print(sys.float_info.min)
print(sys.float_info.dig, sys.float_info.mant_dig, sys.float_info.radix)
print(sys.float_info.max_exp, sys.float_info.min_exp)
print(sys.flags.dont_write_bytecode)
print(sys.flags.debug, sys.flags.utf8_mode)

# --- expect ---
# 2.220446049250313e-16
# 1.7976931348623157e+308
# 2.2250738585072014e-308
# 15 53 2
# 1024 -1021
# 0
# 0 0
