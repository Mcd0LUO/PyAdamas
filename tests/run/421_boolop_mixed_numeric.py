# and/or 的 int|float 混合：结果槽类型是 join 出来的 float，操作数必须先
# 转换再落槽。之前直接存原始机器字：-O0 打出 4.4e-323 这类垃圾 double，
# -O2 常量折叠后生成 movq $9, %xmm8（立即数进 xmm），汇编器直接失败。
# 注：pa 的 float 槽会把整数操作数真的转成 double，所以 9 显示成 9.0，
# 这是兼容性文档里「标注/静态槽决定显示类型」的既有口径。

def truthy_float_int() -> float:
    return 1.5 or 9

def falsy_float_int() -> float:
    return 0.0 or 9

def falsy_int_float() -> float:
    return 0 or 9.5

def falsy_float_zero() -> float:
    return 0.0 or 0

print(truthy_float_int())
print(falsy_float_int())
print(falsy_int_float())
print(falsy_float_zero())
print((1.5 or 9) * 2, (0.0 or 9) * 2)
# --- expect ---
# 1.5
# 9.0
# 9.5
# 0.0
# 3.0 18.0
