# and/or 的 int|float 混合：结果槽类型是 join 出来的 float，操作数必须先
# 转换再落槽。之前直接存原始机器字：-O0 打出 4.4e-323 这类垃圾 double，
# -O2 常量折叠后生成 movq $9, %xmm8（立即数进 xmm），汇编器直接失败。
# 注：**标注为 float 的返回槽**会把整数操作数转成 double，所以 9 显示成 9.0，
# 这是兼容性文档里「标注/静态槽决定显示类型」的既有口径。
# 但没有标注、也没有静态槽的表达式（下面最后一行），类型是**动态**的：
# CPython 里 (0.0 or 9) 是 int 9，(0.0 or 9) * 2 == 18（不是 18.0）。
# 0.672 之前 pa 把它静态推成 float，于是打印 18.0——与 CPython 不一致，已修。

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
# 3.0 18
