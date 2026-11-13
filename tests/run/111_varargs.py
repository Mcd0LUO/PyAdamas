# C 可变参数：printf(fmt, ...) 的整数/浮点/字符串实参分别走整数寄存器、
# xmm 与 data 指针，调用前 %al 记录使用的向量寄存器数。
cimport clib as c

n = c.printf("x=%d y=%.2f s=%s\n", 3, 1.5, "ok")
print(n)

m = c.printf("%d %.1f %.1f %d\n", 1, 2.5, 3.5, 4)
print(m)

# --- expect ---
# x=3 y=1.50 s=ok
# 16
# 1 2.5 3.5 4
# 12
