# 0.564：T[N] 声明数组变量 —— T 是元素宽度，N 是元素个数，符号地址非空。
cimport carrayvar as c

print(c.vals_addr() == c.vals3)

# 符号地址是编译期常量，不需要 None 检查
print(c.vals3[0], c.vals3[1], c.vals3[2])
c.vals3[1] = 99
print(c.vals3[1])

print(c.vals64[0], c.vals64[2])

# --- expect ---
# True
# 10 20 30
# 99
# 100 300
