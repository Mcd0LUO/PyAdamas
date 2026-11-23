# bool 的一元 + / - 结果类型是 int（CPython 语义）：+True == 1、-True == -1，打印为整数。
# 附带算术语义审计中对齐 CPython 的边界：bool 参与算术、负数的 floor 除法/取模、
# float→int 截断、2^53 精度、NaN/inf 比较、算术右移、除零异常。

# 1) 一元 + / -（本次修复路径）
print("pos", +True, +False)
print("neg", -True, -False)
x = -True
print("value", x, x * 3, x + 1)
print("cmp", (-True) == -1, +True == 1, -False == 0)
print("nested", -(True + True), -(-True))

# 2) bool 参与算术与索引
print("bool_arith", True + True, True * 5, 3 + True, 10 - True)
xs = [10, 20, 30]
print("bool_index", xs[True], xs[False])

# 3) 负数的 floor 除法/取模（Python floor 语义）
print("floordiv", -7 // 2, -7 % 2, 7 // -2, 7 % -2)
a = -7
b = 2
print("identity", a == (a // b) * b + a % b)
print("half", int(2.5), int(-2.5), int(0.5), int(-0.5))

# 4) float/int 精度与边界
print("pow53", 2 ** 53 + 1 == 2 ** 53, 2 ** 53 + 1 > 2 ** 53)
print("f53", float(2 ** 53 + 1))
print("int_div", 7 / 2, -7 / 2)

# 5) NaN / inf 比较
inf = float("inf")
nan = inf - inf
print("inf", inf, inf - inf)
print("nan_cmp", nan == nan, nan != nan, nan < 1.0, nan > 1.0)

# 6) 移位与除零（后者应抛异常）
print("shift", -8 >> 1, 1 << 62)
try:
    z = 1 // 0
    print("divzero", z)
except ZeroDivisionError:
    print("divzero_err")

# --- expect ---
# pos 1 0
# neg -1 0
# value -1 -3 0
# cmp True True True
# nested -2 1
# bool_arith 2 5 4 9
# bool_index 20 10
# floordiv -4 1 -4 -1
# identity True
# half 2 -2 0 0
# pow53 False True
# f53 9007199254740992.0
# int_div 3.5 -3.5
# inf inf nan
# nan_cmp False True False False
# shift -4 4611686018427387904
# divzero_err
